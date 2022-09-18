/*
 * Copyright (c) 2023 Pavel Otchertsov <pavel.otchertsov@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "evio.h"

#include <string.h>
#include <limits.h>
#include <assert.h>
#include <malloc.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <stdatomic.h>

#ifndef EVIO_DEF_EVENTS
#define EVIO_DEF_EVENTS 64
#endif

#ifndef EVIO_MAX_EVENTS
#define EVIO_MAX_EVENTS (INT_MAX / (int)sizeof(struct epoll_event))
#endif

#ifndef EVIO_MIN_TIMEJUMP
#define EVIO_MIN_TIMEJUMP EVIO_TIME_FROM_SEC(1)
#endif

#ifndef EVIO_MIN_TIMEOUT
#define EVIO_MIN_TIMEOUT EVIO_TIME_FROM_MSEC(1)
#endif

#ifndef EVIO_DEF_TIMEOUT
#define EVIO_DEF_TIMEOUT EVIO_TIME_FROM_MSEC(59743)
#endif

#ifndef EVIO_MIN_INTERVAL
#define EVIO_MIN_INTERVAL EVIO_TIME_FROM_NSEC(122070)
#endif

#ifndef EVIO_CACHELINE
#define EVIO_CACHELINE 128
#endif

#ifndef __evio_cacheline
#   if __evio_has_attribute(__aligned__)
#       define __evio_cacheline __attribute__((__aligned__(EVIO_CACHELINE)))
#   else
#       define __evio_cacheline
#   endif
#endif

static __evio_nodiscard
void *evio_default_realloc(void *ctx, void *ptr, size_t size)
{
    if (__evio_likely(size)) {
        ptr = realloc(ptr, size);
        if (__evio_unlikely(!ptr))
            abort();
        return ptr;
    }

    free(ptr);
    return NULL;
}

static struct {
    evio_realloc_cb cb;
    void *ctx;
} evio_allocator = {
    .cb = evio_default_realloc,
    .ctx = NULL,
};

void evio_set_allocator(evio_realloc_cb cb, void *ctx)
{
    evio_allocator.cb = cb;
    evio_allocator.ctx = ctx;
}

#define evio_realloc(ptr, size) evio_allocator.cb(evio_allocator.ctx, ptr, size)
#define evio_malloc(size)       evio_realloc(NULL, size)
#define evio_free(ptr)          evio_realloc(ptr, 0)

struct timespec evio_timespec_realtime(void)
{
    static _Atomic clockid_t fast_clock_id = (clockid_t)(-1);

    clockid_t clock_id = atomic_load_explicit(&fast_clock_id, memory_order_relaxed);
    if (__evio_likely(clock_id != (clockid_t)(-1)))
        goto out;

    struct timespec ts;
    if (!clock_getres(CLOCK_REALTIME_COARSE, &ts) && ts.tv_nsec <= 1000000) {
        clock_id = CLOCK_REALTIME_COARSE;
    } else {
        clock_id = CLOCK_REALTIME;
    }

    atomic_store_explicit(&fast_clock_id, clock_id, memory_order_relaxed);
out:
    return evio_timespec(clock_id);
}

struct timespec evio_timespec_monotonic(void)
{
    static _Atomic clockid_t fast_clock_id = (clockid_t)(-1);

    clockid_t clock_id = atomic_load_explicit(&fast_clock_id, memory_order_relaxed);
    if (__evio_likely(clock_id != (clockid_t)(-1)))
        goto out;

    struct timespec ts;
    if (!clock_getres(CLOCK_MONOTONIC_COARSE, &ts) && ts.tv_nsec <= 1000000) {
        clock_id = CLOCK_MONOTONIC_COARSE;
    } else {
        clock_id = CLOCK_MONOTONIC;
    }

    atomic_store_explicit(&fast_clock_id, clock_id, memory_order_relaxed);
out:
    return evio_timespec(clock_id);
}

typedef struct {
    evio_base base;
    evio_time_t at;
} evio_at;

typedef struct {
    evio_at *w;
    evio_time_t at;
} evio_node;

typedef struct {
    evio_base *base;
    uint16_t emask;
} evio_pending;

typedef struct {
    evio_list *head;
    uint8_t emask;
    uint8_t flags;
    uint8_t count;
} evio_fds;

typedef struct {
    evio_list *head;
    _Atomic(evio_loop *) loop;
    _Atomic uint8_t pending;
} __evio_cacheline evio_sig;

static evio_sig signals[NSIG - 1] = { 0 };

struct evio_loop {
    int fd;
    void *data;
    size_t refcount;
    uint8_t done;
    uint8_t reinit;

    evio_time_t time_real;
    evio_time_t time_mono;
    evio_time_t time_base;
    evio_time_t time_diff;
    uint8_t time_negative;

    evio_base pending;
    size_t pending_count;
    size_t pending_total;

    evio_base **reverse;
    size_t reverse_count;
    size_t reverse_total;

    evio_fds *fds;
    size_t fds_total;
    int maxfd;

    int *fdchanges;
    size_t fdchanges_count;
    size_t fdchanges_total;

    evio_poll pipe;
    _Atomic uint8_t pipe_write_wanted;
    _Atomic uint8_t pipe_write_skipped;

    evio_node *timer;
    size_t timer_count;
    size_t timer_total;

    evio_node *cron;
    size_t cron_count;
    size_t cron_total;

    evio_idle **idle;
    size_t idle_count;
    size_t idle_total;

    evio_async **async;
    size_t async_count;
    size_t async_total;
    _Atomic uint8_t async_pending;
    _Atomic uint8_t signal_pending;

    evio_prepare **prepare;
    size_t prepare_count;
    size_t prepare_total;

    evio_check **check;
    size_t check_count;
    size_t check_total;

    evio_cleanup **cleanup;
    size_t cleanup_count;
    size_t cleanup_total;

    struct epoll_event *events;
    size_t events_total;
    int maxevents;
};

static __evio_inline __evio_nonnull(4) __evio_nodiscard
void *evio_array_resize(void *ptr, size_t size, size_t count, size_t *total)
{
    if (__evio_likely(*total >= count))
        return ptr;

    *total = 1ull << (ULLONG_WIDTH - __builtin_clzll(count));
    return evio_realloc(ptr, *total * size);
}

static __evio_inline __evio_nonnull(1, 2)
void evio_list_insert(evio_list **head, evio_list *list)
{
    list->next = *head;
    *head = list;
}

static __evio_inline __evio_nonnull(1, 2)
void evio_list_remove(evio_list **head, evio_list *list)
{
    while (*head && *head != list)
        head = &(*head)->next;
    *head = list->next;
}

#define EVIO_HSIZE      4
#define EVIO_HROOT      (EVIO_HSIZE - 1)
#define EVIO_HPARENT(i) ((((i) - EVIO_HROOT - 1) / EVIO_HSIZE) + EVIO_HROOT)

static __evio_nonnull(1)
void evio_heap_up(evio_node *heap, size_t index)
{
    evio_node node = heap[index];

    while (1) {
        size_t parent = index > EVIO_HROOT ? EVIO_HPARENT(index) : index;

        if (parent == index || heap[parent].at <= node.at)
            break;

        heap[index] = heap[parent];
        heap[index].w->base.active = index;

        index = parent;
    }

    heap[index] = node;
    heap[index].w->base.active = index;
}

static __evio_nonnull(1)
void evio_heap_down(evio_node *heap, size_t index, size_t count)
{
    evio_node node = heap[index];
    evio_node *end = &heap[EVIO_HROOT + count];

    while (1) {
        evio_node *row = &heap[EVIO_HSIZE * (index - EVIO_HROOT) + EVIO_HROOT + 1];
        evio_node *pos = row;

        if (__evio_likely(row + EVIO_HSIZE - 1 < end)) {
            if (pos->at > row[1].at)
                pos = row + 1;
            if (pos->at > row[2].at)
                pos = row + 2;
            if (pos->at > row[3].at)
                pos = row + 3;
        } else if (row < end) {
            if (row + 1 < end && pos->at > row[1].at)
                pos = row + 1;
            if (row + 2 < end && pos->at > row[2].at)
                pos = row + 2;
            if (row + 3 < end && pos->at > row[3].at)
                pos = row + 3;
        } else {
            break;
        }

        if (node.at <= pos->at)
            break;

        heap[index] = *pos;
        heap[index].w->base.active = index;

        index = pos - heap;
    }

    heap[index] = node;
    heap[index].w->base.active = index;
}

static __evio_inline __evio_nonnull(1)
void evio_heap_adjust(evio_node *heap, size_t index, size_t count)
{
    if (index > EVIO_HROOT && heap[index].at <= heap[EVIO_HPARENT(index)].at) {
        evio_heap_up(heap, index);
    } else {
        evio_heap_down(heap, index, count);
    }
}

static __evio_nonnull(1, 2)
void evio_dummy_cb(evio_loop *loop, evio_base *base, uint16_t emask)
{
}

static __evio_nonnull(1)
void evio_pipe_init(evio_loop *loop)
{
    if (__evio_likely(loop->pipe.active))
        return;

    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (__evio_unlikely(fd < 0))
        abort();

    if (loop->pipe.fd >= 0) {
        dup2(fd, loop->pipe.fd);
        close(fd);
        fd = loop->pipe.fd;
    }

    evio_poll_set(&loop->pipe, fd, EVIO_READ);
    evio_poll_start(loop, &loop->pipe);
    evio_loop_unref(loop);
}

static __evio_inline __evio_nonnull(1)
ssize_t evio_pipe_write(evio_loop *loop)
{
    atomic_store_explicit(&loop->pipe_write_skipped, 1, memory_order_release);

    if (!atomic_load_explicit(&loop->pipe_write_wanted, memory_order_seq_cst))
        return 0;

    atomic_store_explicit(&loop->pipe_write_skipped, 0, memory_order_release);

    int err = errno;

    uint64_t counter = 1;
    ssize_t cnt = write(loop->pipe.fd, &counter, sizeof(counter));

    errno = err;
    return cnt;
}

static __evio_inline __evio_nonnull(1)
ssize_t evio_pipe_read(evio_loop *loop)
{
    uint64_t counter;
    return read(loop->pipe.fd, &counter, sizeof(counter));
}

static __evio_nonnull(1, 2)
void evio_pipe_cb(evio_loop *loop, evio_base *base, uint16_t emask)
{
    if (emask & EVIO_READ)
        evio_pipe_read(loop);

    atomic_store_explicit(&loop->pipe_write_skipped, 0, memory_order_release);

    uint8_t signal_expected = 1;
    if (atomic_compare_exchange_strong_explicit(&loop->signal_pending,
                                                &signal_expected, 0,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
        for (int i = NSIG - 1; i--;) {
            evio_sig *sig = &signals[i];

            if (atomic_load_explicit(&sig->loop, memory_order_acquire) != loop)
                continue;

            signal_expected = 1;
            if (atomic_compare_exchange_strong_explicit(&sig->pending,
                                                        &signal_expected, 0,
                                                        memory_order_acq_rel,
                                                        memory_order_relaxed)) {
                for (evio_list *w = sig->head; w; w = w->next)
                    evio_feed_event(loop, &w->base, EVIO_SIGNAL);
            }
        }
    }

    uint8_t async_expected = 1;
    if (atomic_compare_exchange_strong_explicit(&loop->async_pending,
                                                &async_expected, 0,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
        for (size_t i = loop->async_count; i--;) {
            evio_async *async = loop->async[i];

            async_expected = 1;
            if (atomic_compare_exchange_strong_explicit(&async->status,
                                                        &async_expected, 0,
                                                        memory_order_acq_rel,
                                                        memory_order_relaxed)) {
                evio_feed_event(loop, &async->base, EVIO_ASYNC);
            }
        }
    }
}

static __evio_inline __evio_nonnull(1)
void evio_fd_purge(evio_loop *loop, int fd)
{
    evio_fds *fds = &loop->fds[fd];
    evio_poll *w;
    while ((w = (evio_poll *)fds->head)) {
        evio_poll_stop(loop, w);
        evio_feed_event(loop, &w->base, EVIO_ERROR | EVIO_READ | EVIO_WRITE);
    }
}

static __evio_inline __evio_nonnull(1)
void evio_fd_change(evio_loop *loop, int fd, uint8_t flags)
{
    evio_fds *fds = &loop->fds[fd];

    if (!fds->flags) {
        loop->fdchanges = (int *)evio_array_resize(
                              loop->fdchanges, sizeof(int),
                              ++loop->fdchanges_count,
                              &loop->fdchanges_total);
        loop->fdchanges[loop->fdchanges_count - 1] = fd;
    }

    fds->flags |= flags | 1;
}

static __evio_inline __evio_nonnull(1)
void evio_fd_event_nocheck(evio_loop *loop, int fd, uint16_t emask)
{
    evio_fds *fds = &loop->fds[fd];

    for (evio_poll *w = (evio_poll *)fds->head; w; w = (evio_poll *)w->next) {
        if (w->emask & emask)
            evio_feed_event(loop, &w->base, w->emask & emask);
    }
}

static __evio_inline __evio_nonnull(1)
void evio_fd_event(evio_loop *loop, int fd, uint16_t emask)
{
    evio_fds *fds = &loop->fds[fd];

    if (__evio_likely(!fds->flags))
        evio_fd_event_nocheck(loop, fd, emask);
}

static __evio_inline __evio_nonnull(1, 2)
void evio_feed_reverse(evio_loop *loop, evio_base *base)
{
    loop->reverse = (evio_base **)evio_array_resize(
                        loop->reverse, sizeof(evio_base *),
                        ++loop->reverse_count, &loop->reverse_total);
    loop->reverse[loop->reverse_count - 1] = base;
}

static __evio_inline __evio_nonnull(1)
void evio_done_reverse(evio_loop *loop, uint16_t emask)
{
    do {
        evio_base *base = loop->reverse[--loop->reverse_count];
        evio_feed_event(loop, base, emask);
    } while (loop->reverse_count);
}

static __evio_nonnull(1)
void evio_timer_reinit(evio_loop *loop)
{
    if (!loop->timer_count || loop->timer[EVIO_HROOT].at >= loop->time_mono)
        return;

    do {
        evio_timer *w = (evio_timer *)loop->timer[EVIO_HROOT].w;

        if (w->repeat) {
            w->at += w->repeat;
            if (w->at < loop->time_mono)
                w->at = loop->time_mono;

            loop->timer[EVIO_HROOT].at = w->at;
            evio_heap_down(loop->timer, EVIO_HROOT, loop->timer_count);
        } else {
            evio_timer_stop(loop, w);
        }

        evio_feed_reverse(loop, &w->base);
    } while (loop->timer_count && loop->timer[EVIO_HROOT].at < loop->time_mono);

    evio_done_reverse(loop, EVIO_TIMER);
}

static __evio_nonnull(1, 2)
void evio_cron_recalc(evio_loop *loop, evio_cron *w)
{
    evio_time_t interval = w->interval > EVIO_MIN_INTERVAL ?
                           w->interval : EVIO_MIN_INTERVAL;
    evio_time_t at = w->offset + interval * ((loop->time_real - w->offset) / interval);

    while (at <= loop->time_real) {
        evio_time_t next_at = at + w->interval;

        if (__evio_unlikely(next_at == at)) {
            at = loop->time_real;
            break;
        }

        at = next_at;
    }

    w->at = at;
}

static __evio_nonnull(1)
void evio_cron_reinit(evio_loop *loop)
{
    if (!loop->cron_count || loop->cron[EVIO_HROOT].at >= loop->time_real)
        return;

    do {
        evio_cron *w = (evio_cron *)loop->cron[EVIO_HROOT].w;

        if (w->interval) {
            evio_cron_recalc(loop, w);
            loop->cron[EVIO_HROOT].at = w->at;
            evio_heap_down(loop->cron, EVIO_HROOT, loop->cron_count);
        } else {
            evio_cron_stop(loop, w);
        }

        evio_feed_reverse(loop, &w->base);
    } while (loop->cron_count && loop->cron[EVIO_HROOT].at < loop->time_real);

    evio_done_reverse(loop, EVIO_CRON);
}

static __evio_nonnull(1)
void evio_cron_reschedule(evio_loop *loop)
{
    for (size_t i = EVIO_HROOT; i < loop->cron_count + EVIO_HROOT; ++i) {
        evio_cron *w = (evio_cron *)loop->cron[i].w;

        if (w->interval)
            evio_cron_recalc(loop, w);

        loop->cron[i].at = w->at;
    }

    for (size_t i = 0; i < loop->cron_count; ++i)
        evio_heap_up(loop->cron, EVIO_HROOT + i);
}

static __evio_nonnull(1)
void evio_loop_reinit(evio_loop *loop)
{
    if (__evio_likely(!loop->reinit))
        return;

    close(loop->fd);

    loop->fd = epoll_create1(EPOLL_CLOEXEC);
    if (__evio_unlikely(loop->fd < 0))
        abort();

    for (int fd = 0; fd <= loop->maxfd; ++fd) {
        evio_fds *fds = &loop->fds[fd];
        if (fds->emask) {
            fds->emask = 0;
            evio_fd_change(loop, fd, EVIO_POLL);
        }
    }

    if (loop->pipe.active) {
        evio_loop_ref(loop);
        evio_poll_stop(loop, &loop->pipe);

        evio_pipe_init(loop);
        evio_feed_event(loop, &loop->pipe.base, EVIO_CUSTOM);
    }

    loop->reinit = 0;
}

static __evio_nonnull(1)
void evio_epoll(evio_loop *loop, evio_time_t timeout)
{
    timeout = EVIO_TIME_TO_MSEC(timeout);

    if (__evio_unlikely(loop->fdchanges_count)) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));

        for (size_t i = 0; i < loop->fdchanges_count; ++i) {
            int fd = loop->fdchanges[i];
            evio_fds *fds = &loop->fds[fd];

            uint8_t emask = 0;
            for (evio_poll *w = (evio_poll *)fds->head; w; w = (evio_poll *)w->next)
                emask |= (uint8_t)w->emask;

            if (emask && (fds->emask != emask || (fds->flags & EVIO_POLL))) {
                ev.events = ((emask & EVIO_READ)  ? EPOLLIN  : 0) |
                            ((emask & EVIO_WRITE) ? EPOLLOUT : 0);

                ev.data.u64 = ((uint64_t)(uint32_t)fd) |
                              ((uint64_t)(uint32_t)++fds->count << 32);

                int op = fds->emask ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

                if (__evio_unlikely(epoll_ctl(loop->fd, op, fd, &ev))) {
                    if (__evio_unlikely(errno != EEXIST))
                        abort();

                    assert(op == EPOLL_CTL_ADD);

                    if (__evio_unlikely(epoll_ctl(loop->fd, EPOLL_CTL_MOD, fd, &ev)))
                        abort();
                }
            }

            fds->emask = emask & (EVIO_READ | EVIO_WRITE);
            fds->flags = 0;
        }

        loop->fdchanges_count = 0;
    }

    int count = epoll_wait(loop->fd, loop->events, loop->maxevents, timeout);
    if (__evio_unlikely(count < 0)) {
        assert(errno == EINTR);
        return;
    }

    for (size_t i = 0; i < (size_t)count; ++i) {
        struct epoll_event *ev = &loop->events[i];

        int fd = (uint32_t)ev->data.u64;
        evio_fds *fds = &loop->fds[fd];

        if (__evio_unlikely((uint32_t)fds->count != (uint32_t)(ev->data.u64 >> 32))) {
            loop->reinit = 1;
            continue;
        }

        uint8_t emask = (ev->events & (EPOLLIN  | EPOLLERR | EPOLLHUP) ? EVIO_READ  : 0) |
                        (ev->events & (EPOLLOUT | EPOLLERR | EPOLLHUP) ? EVIO_WRITE : 0);

        if (__evio_unlikely(emask & ~fds->emask)) {
            ev->events = ((fds->emask & EVIO_READ)  ? EPOLLIN  : 0) |
                         ((fds->emask & EVIO_WRITE) ? EPOLLOUT : 0);

            int op = fds->emask ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;

            if (__evio_unlikely(epoll_ctl(loop->fd, op, fd, ev))) {
                loop->reinit = 1;
                continue;
            }
        }

        evio_fd_event(loop, fd, emask);
    }

    if (__evio_unlikely(count == loop->maxevents &&
                        count < EVIO_MAX_EVENTS)) {
        loop->events = (struct epoll_event *)evio_array_resize(
                           loop->events, sizeof(struct epoll_event),
                           (size_t)count + 1, &loop->events_total);
        loop->maxevents = loop->events_total < EVIO_MAX_EVENTS ?
                          loop->events_total : EVIO_MAX_EVENTS;
    }
}

static __evio_inline __evio_nonnull(1, 2)
void evio_feed_events(evio_loop *loop, evio_base **base, size_t count, uint16_t emask)
{
    for (size_t i = 0; i < count; ++i)
        evio_feed_event(loop, base[i], emask);
}

static __evio_inline __evio_nonnull(1)
void evio_loop_time_init(evio_loop *loop, evio_time_t mono)
{
    loop->time_real = evio_time();
    loop->time_base = loop->time_mono = mono;

    if (loop->time_real > loop->time_mono) {
        loop->time_diff = loop->time_real - loop->time_mono;
        loop->time_negative = 0;
    } else {
        loop->time_diff = loop->time_mono - loop->time_real;
        loop->time_negative = 1;
    }
}

evio_loop *evio_loop_new(int maxevents)
{
    int fd = epoll_create1(EPOLL_CLOEXEC);
    if (__evio_unlikely(fd < 0))
        return NULL;

    evio_loop *loop = (evio_loop *)evio_malloc(sizeof(evio_loop));
    *loop = (evio_loop) {
        .fd     = fd,
        .maxfd  = -1,
    };

    evio_loop_time_init(loop, evio_monotonic_time());

    evio_base_init(&loop->pending, evio_dummy_cb);
    loop->pending.data = NULL;

    evio_base_init(&loop->pipe.base, evio_pipe_cb);
    loop->pipe.data = NULL;
    loop->pipe.fd = -1;

    loop->maxevents = maxevents > 0 &&
                      maxevents <= EVIO_MAX_EVENTS ?
                      maxevents  : EVIO_DEF_EVENTS;
    loop->events = (struct epoll_event *)evio_array_resize(
                       loop->events, sizeof(struct epoll_event),
                       loop->maxevents, &loop->events_total);
    return loop;
}

void evio_loop_free(evio_loop *loop)
{
    if (__evio_unlikely(!loop))
        return;

    if (loop->cleanup_count) {
        evio_feed_events(loop, (evio_base **)loop->cleanup,
                         loop->cleanup_count, EVIO_CLEANUP);
        evio_invoke_pending(loop);
    }

    for (int i = NSIG - 1; i--;) {
        evio_sig *sig = &signals[i];

        if (atomic_load_explicit(&sig->loop, memory_order_acquire) == loop) {
            sig->head = NULL;

            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = SIG_DFL;

            if (__evio_unlikely(sigaction(i + 1, &sa, NULL)))
                abort();

            atomic_store_explicit(&sig->loop, NULL, memory_order_release);
        }
    }

    if (loop->pipe.fd >= 0) {
        close(loop->pipe.fd);
        loop->pipe.fd = -1;
    }

    if (loop->fd >= 0) {
        close(loop->fd);
        loop->fd = -1;
    }

    evio_free(loop->pending.data);
    evio_free(loop->reverse);
    evio_free(loop->fds);
    evio_free(loop->fdchanges);
    evio_free(loop->timer);
    evio_free(loop->cron);
    evio_free(loop->idle);
    evio_free(loop->async);
    evio_free(loop->prepare);
    evio_free(loop->check);
    evio_free(loop->cleanup);
    evio_free(loop->events);
    evio_free(loop);
}

evio_time_t evio_loop_time(evio_loop *loop)
{
    return loop->time_real;
}

evio_time_t evio_loop_monotonic_time(evio_loop *loop)
{
    return loop->time_mono;
}

void evio_loop_update_time(evio_loop *loop)
{
    loop->time_mono = evio_monotonic_time();

    if (__evio_likely(loop->time_mono > loop->time_base &&
                      loop->time_mono - loop->time_base < EVIO_MIN_TIMEJUMP / 2)) {
        if (loop->time_negative) {
            loop->time_real = loop->time_mono - loop->time_diff;
        } else {
            loop->time_real = loop->time_mono + loop->time_diff;
        }
        return;
    }

    evio_time_t time_diff = loop->time_diff;
    evio_loop_time_init(loop, loop->time_mono);

    for (int i = 4; --i;) {
        evio_time_t jump = time_diff > loop->time_diff
                           ? time_diff - loop->time_diff
                           : loop->time_diff - time_diff;
        if (__evio_likely(jump < EVIO_MIN_TIMEJUMP))
            return;

        evio_loop_time_init(loop, evio_monotonic_time());
    }

    evio_cron_reschedule(loop);
}

void evio_loop_ref(evio_loop *loop)
{
    if (__evio_unlikely(++loop->refcount == 0))
        abort();
}

void evio_loop_unref(evio_loop *loop)
{
    if (__evio_unlikely(loop->refcount-- == 0))
        abort();
}

size_t evio_loop_refcount(evio_loop *loop)
{
    return loop->refcount;
}

void *evio_loop_set_data(evio_loop *loop, void *data)
{
    void *ptr = loop->data;
    loop->data = data;
    return ptr;
}

void *evio_loop_get_data(evio_loop *loop)
{
    return loop->data;
}

static __evio_inline __evio_nonnull(1) __evio_nodiscard
evio_time_t evio_loop_timeout(evio_loop *loop)
{
    if (loop->done || !loop->refcount ||
        loop->pending_count ||
        loop->idle_count) {
        return 0;
    }

    evio_time_t timeout = EVIO_DEF_TIMEOUT;

    if (loop->timer_count) {
        evio_node *node = &loop->timer[EVIO_HROOT];
        evio_time_t diff = node->at > loop->time_mono ?
                           node->at - loop->time_mono : 0;
        if (timeout > diff)
            timeout = diff;
    }

    if (loop->cron_count) {
        evio_node *node = &loop->cron[EVIO_HROOT];
        evio_time_t diff = node->at > loop->time_real ?
                           node->at - loop->time_real : 0;
        if (timeout > diff)
            timeout = diff;
    }

    if (__evio_unlikely(timeout && timeout < EVIO_MIN_TIMEOUT))
        timeout = EVIO_MIN_TIMEOUT;

    return timeout;
}

size_t evio_loop_run(evio_loop *loop, uint8_t run_mode)
{
    run_mode &= EVIO_RUN_NOWAIT | EVIO_RUN_ONCE;
    loop->done = EVIO_BREAK_CANCEL;
    evio_invoke_pending(loop);

    do {
        if (loop->prepare_count) {
            evio_feed_events(loop, (evio_base **)loop->prepare,
                             loop->prepare_count, EVIO_PREPARE);
            evio_invoke_pending(loop);
        }

        if (__evio_unlikely(loop->done))
            break;

        evio_loop_reinit(loop);
        evio_loop_update_time(loop);

        atomic_store_explicit(&loop->pipe_write_wanted, 1, memory_order_seq_cst);

        evio_time_t timeout = (run_mode & EVIO_RUN_NOWAIT)
                              ? 0 : evio_loop_timeout(loop);
        evio_epoll(loop, timeout);

        atomic_store_explicit(&loop->pipe_write_wanted, 0, memory_order_relaxed);

        if (atomic_load_explicit(&loop->pipe_write_skipped, memory_order_acquire)) {
            assert(&loop->pipe.active);
            evio_feed_event(loop, &loop->pipe.base, EVIO_CUSTOM);
        }

        evio_loop_update_time(loop);
        evio_timer_reinit(loop);
        evio_cron_reinit(loop);

        if (loop->idle_count && !loop->pending_count) {
            evio_feed_events(loop, (evio_base **)loop->idle,
                             loop->idle_count, EVIO_IDLE);
        }

        if (loop->check_count) {
            evio_feed_events(loop, (evio_base **)loop->check,
                             loop->check_count, EVIO_CHECK);
        }

        evio_invoke_pending(loop);
    } while (__evio_likely(
                 loop->refcount > 0 &&
                 loop->done == EVIO_BREAK_CANCEL &&
                 run_mode == EVIO_RUN_DEFAULT
             ));

    if (loop->done == EVIO_BREAK_ONE)
        loop->done = EVIO_BREAK_CANCEL;

    return loop->refcount;
}

void evio_loop_break(evio_loop *loop, uint8_t break_mode)
{
    loop->done = break_mode & (EVIO_BREAK_ONE | EVIO_BREAK_ALL);
}

void evio_loop_walk(evio_loop *loop, evio_cb cb, uint16_t emask)
{
    if (emask & EVIO_POLL) {
        for (int fd = 0; fd <= loop->maxfd; ++fd) {
            evio_fds *fds = &loop->fds[fd];

            for (evio_list *w = fds->head; w; w = w->next) {
                if (__evio_likely(w->cb != evio_pipe_cb))
                    cb(loop, &w->base, EVIO_WALK | EVIO_POLL);
            }
        }
    }

    if (emask & EVIO_TIMER) {
        for (size_t i = loop->timer_count + EVIO_HROOT; i-- > EVIO_HROOT;) {
            evio_timer *w = (evio_timer *)loop->timer[i].w;
            cb(loop, &w->base, EVIO_WALK | EVIO_TIMER);
        }
    }

    if (emask & EVIO_CRON) {
        for (size_t i = loop->cron_count + EVIO_HROOT; i-- > EVIO_HROOT;) {
            evio_cron *w = (evio_cron *)loop->cron[i].w;
            cb(loop, &w->base, EVIO_WALK | EVIO_CRON);
        }
    }

    if (emask & EVIO_SIGNAL) {
        for (int i = NSIG - 1; i--;) {
            evio_sig *sig = &signals[i];

            if (atomic_load_explicit(&sig->loop, memory_order_acquire) != loop)
                continue;

            for (evio_list *w = sig->head; w; w = w->next)
                cb(loop, &w->base, EVIO_WALK | EVIO_SIGNAL);
        }
    }

    if (emask & EVIO_ASYNC) {
        for (size_t i = loop->async_count; i--;) {
            evio_async *w = loop->async[i];
            cb(loop, &w->base, EVIO_WALK | EVIO_ASYNC);
        }
    }

    if (emask & EVIO_IDLE) {
        for (size_t i = loop->idle_count; i--;) {
            evio_idle *w = loop->idle[i];
            cb(loop, &w->base, EVIO_WALK | EVIO_IDLE);
        }
    }

    if (emask & EVIO_PREPARE) {
        for (size_t i = loop->prepare_count; i--;) {
            evio_prepare *w = loop->prepare[i];
            cb(loop, &w->base, EVIO_WALK | EVIO_PREPARE);
        }
    }

    if (emask & EVIO_CHECK) {
        for (size_t i = loop->check_count; i--;) {
            evio_check *w = loop->check[i];
            cb(loop, &w->base, EVIO_WALK | EVIO_CHECK);
        }
    }

    if (emask & EVIO_CLEANUP) {
        for (size_t i = loop->cleanup_count; i--;) {
            evio_cleanup *w = loop->cleanup[i];
            cb(loop, &w->base, EVIO_WALK | EVIO_CLEANUP);
        }
    }
}

void evio_feed_event(evio_loop *loop, evio_base *base, uint16_t emask)
{
    if (__evio_unlikely(base->pending)) {
        evio_pending *p = (evio_pending *)loop->pending.data + base->pending - 1;
        p->emask |= emask;
    } else {
        base->pending = ++loop->pending_count;
        loop->pending.data = evio_array_resize(
                                 loop->pending.data, sizeof(evio_pending),
                                 loop->pending_count, &loop->pending_total);
        evio_pending *p = (evio_pending *)loop->pending.data + base->pending - 1;
        p->base = base;
        p->emask = emask;
    }
}

void evio_feed_fd_event(evio_loop *loop, int fd, uint16_t emask)
{
    if (__evio_likely(fd >= 0 && fd <= loop->maxfd))
        evio_fd_event_nocheck(loop, fd, emask);
}

void evio_feed_fd_close(evio_loop *loop, int fd)
{
    if (__evio_likely(fd >= 0 && fd <= loop->maxfd))
        evio_fd_purge(loop, fd);
}

void evio_feed_signal(int signum)
{
    if (__evio_unlikely(signum <= 0 || signum >= NSIG))
        return;

    evio_sig *sig = &signals[signum - 1];

    evio_loop *loop = atomic_load_explicit(&sig->loop, memory_order_acquire);
    if (__evio_unlikely(!loop))
        return;

    atomic_store_explicit(&sig->pending, 1, memory_order_release);

    uint8_t signal_expected = 0;
    if (atomic_compare_exchange_strong_explicit(&loop->signal_pending,
                                                &signal_expected, 1,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
        evio_pipe_write(loop);
    }
}

void evio_feed_signal_event(evio_loop *loop, int signum)
{
    if (__evio_unlikely(signum <= 0 || signum >= NSIG))
        return;

    evio_sig *sig = &signals[signum - 1];

    evio_loop *ptr = atomic_load_explicit(&sig->loop, memory_order_acquire);
    if (__evio_unlikely(!ptr || ptr != loop))
        return;

    atomic_store_explicit(&sig->pending, 0, memory_order_release);

    for (evio_list *w = sig->head; w; w = w->next)
        evio_feed_event(loop, &w->base, EVIO_SIGNAL);
}

void evio_invoke_pending(evio_loop *loop)
{
    while (loop->pending_count) {
        evio_pending *p = (evio_pending *)loop->pending.data + --loop->pending_count;
        p->base->pending = 0;
        p->base->cb(loop, p->base, p->emask);
    }
}

static __evio_inline __evio_nonnull(1, 2)
void evio_clear_pending(evio_loop *loop, evio_base *base)
{
    if (base->pending) {
        evio_pending *p = (evio_pending *)loop->pending.data + base->pending - 1;
        p->base = &loop->pending;
        base->pending = 0;
    }
}

void evio_poll_start(evio_loop *loop, evio_poll *w)
{
    if (__evio_unlikely(w->active))
        return;

    if (__evio_unlikely(w->fd < 0))
        abort();

    w->active = 1;
    evio_loop_ref(loop);

    loop->fds = (evio_fds *)evio_array_resize(
                    loop->fds, sizeof(evio_fds),
                    (size_t)w->fd + 1, &loop->fds_total);

    if (w->fd > loop->maxfd) {
        size_t index = loop->maxfd + 1;
        size_t count = loop->fds_total - loop->maxfd - 1;
        memset(&loop->fds[index], 0, count * sizeof(evio_fds));
        loop->maxfd = w->fd;
    }

    evio_fds *fds = &loop->fds[w->fd];
    evio_list_insert(&fds->head, &w->list);

    evio_fd_change(loop, w->fd, w->emask & EVIO_POLL);
    w->emask &= ~EVIO_POLL;
}

void evio_poll_stop(evio_loop *loop, evio_poll *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    if (__evio_unlikely(w->fd < 0 || w->fd > loop->maxfd))
        abort();

    evio_fds *fds = &loop->fds[w->fd];
    evio_list_remove(&fds->head, &w->list);

    evio_loop_unref(loop);
    w->active = 0;

    evio_fd_change(loop, w->fd, 0);
}

void evio_timer_start(evio_loop *loop, evio_timer *w)
{
    if (__evio_unlikely(w->active))
        return;

    w->at += loop->time_mono;

    w->active = ++loop->timer_count + EVIO_HROOT - 1;
    evio_loop_ref(loop);

    loop->timer = (evio_node *)evio_array_resize(
                      loop->timer, sizeof(evio_node),
                      w->active + 1, &loop->timer_total);

    evio_node *node = &loop->timer[w->active];
    node->w = (evio_at *)w;
    node->at = w->at;

    evio_heap_up(loop->timer, w->active);
}

void evio_timer_stop(evio_loop *loop, evio_timer *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    assert(loop->timer[w->active].w == (evio_at *)w);
    size_t count = --loop->timer_count;

    if (__evio_likely(w->active < count + EVIO_HROOT)) {
        loop->timer[w->active] = loop->timer[count + EVIO_HROOT];
        evio_heap_adjust(loop->timer, w->active, count);
    }

    if (__evio_likely(w->at > loop->time_mono)) {
        w->at -= loop->time_mono;
    } else {
        w->at = 0;
    }

    evio_loop_unref(loop);
    w->active = 0;
}

void evio_timer_again(evio_loop *loop, evio_timer *w)
{
    evio_clear_pending(loop, &w->base);

    if (w->active) {
        if (w->repeat) {
            w->at = loop->time_mono + w->repeat;
            loop->timer[w->active].at = w->at;
            evio_heap_adjust(loop->timer, w->active, loop->timer_count);
        } else {
            evio_timer_stop(loop, w);
        }
    } else if (w->repeat) {
        w->at = w->repeat;
        evio_timer_start(loop, w);
    }
}

evio_time_t evio_timer_remaining(evio_loop *loop, evio_timer *w)
{
    if (w->active && __evio_likely(w->at >= loop->time_mono)) {
        return w->at - loop->time_mono;
    } else {
        return 0;
    }
}

void evio_cron_start(evio_loop *loop, evio_cron *w)
{
    if (__evio_unlikely(w->active))
        return;

    if (w->interval) {
        evio_cron_recalc(loop, w);
    } else {
        w->at = w->offset;
    }

    w->active = ++loop->cron_count + EVIO_HROOT - 1;
    evio_loop_ref(loop);

    loop->cron = (evio_node *)evio_array_resize(
                     loop->cron, sizeof(evio_node),
                     w->active + 1, &loop->cron_total);

    evio_node *node = &loop->cron[w->active];
    node->w = (evio_at *)w;
    node->at = w->at;

    evio_heap_up(loop->cron, w->active);
}

void evio_cron_stop(evio_loop *loop, evio_cron *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    assert(loop->cron[w->active].w == (evio_at *)w);
    size_t count = --loop->cron_count;

    if (__evio_likely(w->active < count + EVIO_HROOT)) {
        loop->cron[w->active] = loop->cron[count + EVIO_HROOT];
        evio_heap_adjust(loop->cron, w->active, count);
    }

    evio_loop_unref(loop);
    w->active = 0;
}

void evio_cron_again(evio_loop *loop, evio_cron *w)
{
    evio_cron_stop(loop, w);
    evio_cron_start(loop, w);
}

void evio_signal_start(evio_loop *loop, evio_signal *w)
{
    if (__evio_unlikely(w->active))
        return;

    if (__evio_unlikely(w->signum <= 0 || w->signum >= NSIG))
        abort();

    evio_sig *sig = &signals[w->signum - 1];

    evio_loop *ptr = NULL;
    if (!atomic_compare_exchange_strong_explicit(&sig->loop, &ptr, loop,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        if (__evio_unlikely(ptr != loop))
            abort();
    }

    evio_list_insert(&sig->head, &w->list);

    w->active = 1;
    evio_loop_ref(loop);

    if (!w->next) {
        evio_pipe_init(loop);

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = evio_feed_signal;
        sa.sa_flags = SA_RESTART;

        if (__evio_unlikely(sigfillset(&sa.sa_mask)))
            abort();

        if (__evio_unlikely(sigaction(w->signum, &sa, NULL)))
            abort();
    }
}

void evio_signal_stop(evio_loop *loop, evio_signal *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    if (__evio_unlikely(w->signum <= 0 || w->signum >= NSIG))
        abort();

    evio_sig *sig = &signals[w->signum - 1];

    evio_loop *ptr = atomic_load_explicit(&sig->loop, memory_order_acquire);
    if (__evio_unlikely(!ptr || ptr != loop))
        abort();

    evio_list_remove(&sig->head, &w->list);

    if (!sig->head) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;

        if (__evio_unlikely(sigaction(w->signum, &sa, NULL)))
            abort();

        atomic_store_explicit(&sig->loop, NULL, memory_order_release);
    }

    evio_loop_unref(loop);
    w->active = 0;
}

void evio_async_start(evio_loop *loop, evio_async *w)
{
    if (__evio_unlikely(w->active))
        return;

    w->status = 0;
    evio_pipe_init(loop);

    w->active = ++loop->async_count;
    evio_loop_ref(loop);

    loop->async = (evio_async **)evio_array_resize(
                      loop->async, sizeof(evio_async *),
                      loop->async_count, &loop->async_total);
    loop->async[w->active - 1] = w;
}

void evio_async_stop(evio_loop *loop, evio_async *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    loop->async[w->active - 1] = loop->async[--loop->async_count];
    loop->async[w->active - 1]->active = w->active;

    evio_loop_unref(loop);
    w->active = 0;
}

void evio_async_send(evio_loop *loop, evio_async *w)
{
    atomic_store_explicit(&w->status, 1, memory_order_release);

    uint8_t async_expected = 0;
    if (atomic_compare_exchange_strong_explicit(&loop->async_pending,
                                                &async_expected, 1,
                                                memory_order_acq_rel,
                                                memory_order_relaxed)) {
        evio_pipe_write(loop);
    }
}

void evio_idle_start(evio_loop *loop, evio_idle *w)
{
    if (__evio_unlikely(w->active))
        return;

    w->active = ++loop->idle_count;
    evio_loop_ref(loop);

    loop->idle = (evio_idle **)evio_array_resize(
                     loop->idle, sizeof(evio_idle *),
                     loop->idle_count, &loop->idle_total);
    loop->idle[w->active - 1] = w;
}

void evio_idle_stop(evio_loop *loop, evio_idle *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    loop->idle[w->active - 1] = loop->idle[--loop->idle_count];
    loop->idle[w->active - 1]->active = w->active;

    evio_loop_unref(loop);
    w->active = 0;
}

void evio_prepare_start(evio_loop *loop, evio_prepare *w)
{
    if (__evio_unlikely(w->active))
        return;

    w->active = ++loop->prepare_count;
    evio_loop_ref(loop);

    loop->prepare = (evio_prepare **)evio_array_resize(
                        loop->prepare, sizeof(evio_prepare *),
                        loop->prepare_count, &loop->prepare_total);
    loop->prepare[w->active - 1] = w;
}

void evio_prepare_stop(evio_loop *loop, evio_prepare *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    loop->prepare[w->active - 1] = loop->prepare[--loop->prepare_count];
    loop->prepare[w->active - 1]->active = w->active;

    evio_loop_unref(loop);
    w->active = 0;
}

void evio_check_start(evio_loop *loop, evio_check *w)
{
    if (__evio_unlikely(w->active))
        return;

    w->active = ++loop->check_count;
    evio_loop_ref(loop);

    loop->check = (evio_check **)evio_array_resize(
                      loop->check, sizeof(evio_check *),
                      loop->check_count, &loop->check_total);
    loop->check[w->active - 1] = w;
}

void evio_check_stop(evio_loop *loop, evio_check *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    loop->check[w->active - 1] = loop->check[--loop->check_count];
    loop->check[w->active - 1]->active = w->active;

    evio_loop_unref(loop);
    w->active = 0;
}

void evio_cleanup_start(evio_loop *loop, evio_cleanup *w)
{
    if (__evio_unlikely(w->active))
        return;

    w->active = ++loop->cleanup_count;
    //evio_loop_ref(loop);

    loop->cleanup = (evio_cleanup **)evio_array_resize(
                        loop->cleanup, sizeof(evio_cleanup *),
                        loop->cleanup_count, &loop->cleanup_total);
    loop->cleanup[w->active - 1] = w;
}

void evio_cleanup_stop(evio_loop *loop, evio_cleanup *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    loop->cleanup[w->active - 1] = loop->cleanup[--loop->cleanup_count];
    loop->cleanup[w->active - 1]->active = w->active;

    //evio_loop_unref(loop);
    w->active = 0;
}
