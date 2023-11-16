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

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#define EVIO_DEF_EVENTS 64
#define EVIO_MAX_EVENTS (INT_MAX / (int)sizeof(struct epoll_event))
#define EVIO_FLAG 0xF0

#ifndef EVIO_CACHELINE
#define EVIO_CACHELINE 128
#endif

#ifndef __evio_has_builtin
#   ifdef __has_builtin
#       define __evio_has_builtin(x) __has_builtin(x)
#   else
#       define __evio_has_builtin(x) (0)
#   endif
#endif

#ifndef __evio_likely
#   if __evio_has_builtin(__builtin_expect)
#       define __evio_likely(x) (__builtin_expect(!!(x), 1))
#   else
#       define __evio_likely(x) (x)
#   endif
#endif

#ifndef __evio_unlikely
#   if __evio_has_builtin(__builtin_expect)
#       define __evio_unlikely(x) (__builtin_expect(!!(x), 0))
#   else
#       define __evio_unlikely(x) (x)
#   endif
#endif

#ifndef __evio_noinline
#   if __evio_has_attribute(__noinline__)
#       define __evio_noinline __attribute__((__noinline__))
#   else
#       define __evio_noinline
#   endif
#endif

#ifndef __evio_aligned
#   if __evio_has_attribute(__aligned__)
#       define __evio_aligned(x) __attribute__((__aligned__(x)))
#   else
#       define __evio_aligned(x)
#   endif
#endif

static __evio_nodiscard
void *evio_default_realloc(void *ctx, void *ptr, size_t size)
{
    if (__evio_likely(size)) {
        ctx = realloc(ptr, size);
        if (__evio_unlikely(!ctx))
            abort();
        return ctx;
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

typedef struct {
    evio_timer *w;
    uint64_t time;
} evio_node;

typedef struct {
    evio_base *base;
    uint16_t emask;
} evio_pending;

typedef struct {
    evio_list *head;
    uint8_t emask;
    uint8_t cache;
    uint8_t flags;
    uint8_t value;
} evio_fds;

typedef struct {
    evio_list *head;
    _Atomic(evio_loop *) loop;
    _Atomic uint8_t pending;
    struct sigaction sa_old;
} __evio_aligned(EVIO_CACHELINE) evio_sig;

static evio_sig signals[NSIG - 1] = { 0 };

struct evio_loop {
    int fd;
    void *data;
    size_t refcount;

    uint64_t time;
    clockid_t clock_id;

    uint8_t done;
    uint8_t reinit;

    evio_base pending;
    size_t pending_count;
    size_t pending_total;

    evio_poll event;
    _Atomic uint8_t event_pending;
    _Atomic uint8_t eventfd_allow;
    _Atomic uint8_t async_pending;
    _Atomic uint8_t signal_pending;

    evio_fds *fds;
    size_t fds_total;
    int maxfd;

    int *fdchanges;
    size_t fdchanges_count;
    size_t fdchanges_total;

    int *fderrors;
    size_t fderrors_count;
    size_t fderrors_total;

    evio_node *timer;
    size_t timer_count;
    size_t timer_total;

    evio_idle **idle;
    size_t idle_count;
    size_t idle_total;

    evio_async **async;
    size_t async_count;
    size_t async_total;

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

    sigset_t epoll_sigmask;
};

static __evio_inline __evio_nonnull(4) __evio_nodiscard
void *evio_array_resize(void *ptr, size_t size, size_t count, size_t *total)
{
    if (__evio_likely(*total >= count))
        return ptr;

    *total = 1ULL << (sizeof(unsigned long long) * 8 - __builtin_clzll(count));
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

#define EVIO_HSIZE      3
#define EVIO_HROOT      (EVIO_HSIZE - 1)
#define EVIO_HPARENT(i) ((((i) - EVIO_HROOT - 1) / EVIO_HSIZE) + EVIO_HROOT)

static __evio_noinline __evio_nonnull(1)
void evio_heap_up(evio_node *heap, size_t index)
{
    evio_node node = heap[index];

    while (1) {
        size_t parent = index > EVIO_HROOT ? EVIO_HPARENT(index) : index;

        if (parent == index || heap[parent].time <= node.time)
            break;

        heap[index] = heap[parent];
        heap[index].w->active = index;

        index = parent;
    }

    heap[index] = node;
    heap[index].w->active = index;
}

static __evio_noinline __evio_nonnull(1)
void evio_heap_down(evio_node *heap, size_t index, size_t count)
{
    evio_node node = heap[index];
    evio_node *end = &heap[EVIO_HROOT + count];

    while (1) {
        evio_node *row = &heap[EVIO_HSIZE * (index - EVIO_HROOT) + EVIO_HROOT + 1];
        evio_node *pos = row;

        if (__evio_likely(row + EVIO_HSIZE - 1 < end)) {
            for (size_t i = 1; i < EVIO_HSIZE; ++i) {
                if (pos->time > row[i].time)
                    pos = row + i;
            }
        } else if (row < end) {
            for (size_t i = 1; i < EVIO_HSIZE && row + i < end; ++i) {
                if (pos->time > row[i].time)
                    pos = row + i;
            }
        } else {
            break;
        }

        if (node.time <= pos->time)
            break;

        heap[index] = *pos;
        heap[index].w->active = index;

        index = pos - heap;
    }

    heap[index] = node;
    heap[index].w->active = index;
}

static __evio_inline __evio_nonnull(1)
void evio_heap_adjust(evio_node *heap, size_t index, size_t count)
{
    if (index > EVIO_HROOT && heap[index].time <= heap[EVIO_HPARENT(index)].time) {
        evio_heap_up(heap, index);
    } else {
        evio_heap_down(heap, index, count);
    }
}

static __evio_noinline __evio_nonnull(1, 2)
void evio_dummy_cb(evio_loop *loop, evio_base *base, uint16_t emask)
{
}

static __evio_inline __evio_nonnull(1, 2)
void evio_queue_event(evio_loop *loop, evio_base *base, uint16_t emask)
{
    if (__evio_unlikely(base->pending)) {
        evio_pending *p = (evio_pending *)loop->pending.data + base->pending - 1;
        p->emask |= emask;
        return;
    }

    base->pending = ++loop->pending_count;
    loop->pending.data = evio_array_resize(
                             loop->pending.data, sizeof(evio_pending),
                             loop->pending_count, &loop->pending_total);

    evio_pending *p = (evio_pending *)loop->pending.data + base->pending - 1;
    p->base = base;
    p->emask = emask;
}

static __evio_noinline __evio_nonnull(1, 2)
void evio_queue_events(evio_loop *loop, evio_base **base, size_t count, uint16_t emask)
{
    for (size_t i = count; i--;)
        evio_queue_event(loop, base[i], emask);
}

static __evio_noinline __evio_nonnull(1)
void evio_queue_fd_events(evio_loop *loop, int fd, uint8_t emask)
{
    evio_fds *fds = &loop->fds[fd];

    for (evio_poll *w = (evio_poll *)fds->head; w; w = (evio_poll *)w->next) {
        if (w->emask & emask)
            evio_queue_event(loop, &w->base, EVIO_POLL | (w->emask & emask));
    }
}

static __evio_noinline __evio_nonnull(1)
void evio_queue_fd_errors(evio_loop *loop, int fd)
{
    evio_fds *fds = &loop->fds[fd];

    while (fds->head) {
        evio_poll *w = (evio_poll *)fds->head;
        evio_poll_stop(loop, w);
        evio_queue_event(loop, &w->base, EVIO_POLL | EVIO_READ | EVIO_WRITE | EVIO_ERROR);
    }
}

static __evio_inline __evio_nonnull(1)
void evio_queue_fd_change(evio_loop *loop, int fd, uint8_t flags)
{
    evio_fds *fds = &loop->fds[fd];

    if (__evio_likely(!fds->flags)) {
        loop->fdchanges = (int *)evio_array_resize(
                              loop->fdchanges, sizeof(int),
                              ++loop->fdchanges_count,
                              &loop->fdchanges_total);
        loop->fdchanges[loop->fdchanges_count - 1] = fd;
    }

    fds->flags |= EVIO_FLAG | flags;
}

static __evio_inline __evio_nonnull(1)
int evio_eventfd_write(evio_loop *loop)
{
    if (atomic_exchange_explicit(&loop->event_pending, 1, memory_order_acq_rel))
        return 0;

    if (!atomic_load_explicit(&loop->eventfd_allow, memory_order_seq_cst))
        return 0;

    atomic_store_explicit(&loop->event_pending, 0, memory_order_release);

    int err = errno;
    int rc = eventfd_write(loop->event.fd, 1);
    errno = err;
    return rc;
}

static __evio_inline __evio_nonnull(1)
int evio_eventfd_read(evio_loop *loop)
{
    eventfd_t counter;
    return eventfd_read(loop->event.fd, &counter);
}

static __evio_noinline __evio_nonnull(1, 2)
void evio_eventfd_cb(evio_loop *loop, evio_base *base, uint16_t emask)
{
    atomic_store_explicit(&loop->event_pending, 0, memory_order_release);

    if (emask & EVIO_READ)
        evio_eventfd_read(loop);

    if (atomic_exchange_explicit(&loop->signal_pending, 0, memory_order_acq_rel)) {
        for (int i = NSIG - 1; i--;) {
            evio_sig *sig = &signals[i];

            if (atomic_load_explicit(&sig->loop, memory_order_acquire) != loop)
                continue;

            if (atomic_exchange_explicit(&sig->pending, 0, memory_order_acq_rel)) {
                for (evio_list *w = sig->head; w; w = w->next)
                    evio_queue_event(loop, &w->base, EVIO_SIGNAL);
            }
        }
    }

    if (atomic_exchange_explicit(&loop->async_pending, 0, memory_order_acq_rel)) {
        for (size_t i = loop->async_count; i--;) {
            evio_async *async = loop->async[i];

            if (atomic_exchange_explicit(&async->status, 0, memory_order_acq_rel))
                evio_queue_event(loop, &async->base, EVIO_ASYNC);
        }
    }
}

static __evio_noinline __evio_nonnull(1)
void evio_eventfd_init(evio_loop *loop)
{
    if (__evio_likely(loop->event.active))
        return;

    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (__evio_unlikely(fd < 0))
        abort();

    if (loop->event.fd >= 0) {
        int rc = dup3(fd, loop->event.fd, O_CLOEXEC);
        close(fd);

        if (__evio_unlikely(rc))
            abort();

        fd = loop->event.fd;
    }

    evio_poll_set(&loop->event, fd, EVIO_READ);
    evio_poll_start(loop, &loop->event);
    evio_unref(loop);
}

static __evio_noinline __evio_nonnull(1)
void evio_poll_update(evio_loop *loop)
{
    if (__evio_likely(!loop->fdchanges_count))
        return;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    do {
        int fd = loop->fdchanges[--loop->fdchanges_count];
        evio_fds *fds = &loop->fds[fd];

        uint8_t flags = fds->flags;
        fds->flags = 0;

        uint8_t emask = fds->emask;
        fds->emask = 0;

        for (evio_poll *w = (evio_poll *)fds->head; w; w = (evio_poll *)w->next)
            fds->emask |= w->emask;

        fds->emask &= EVIO_READ | EVIO_WRITE;

        if (!fds->emask)
            continue;

        if (fds->emask == emask && !(flags & EVIO_POLL))
            continue;

        uint8_t cache = fds->cache;
        fds->cache = fds->emask;

        ev.events = ((fds->emask & EVIO_READ)  ? EPOLLIN  : 0) |
                    ((fds->emask & EVIO_WRITE) ? EPOLLOUT : 0);

        ev.data.u64 = ((uint64_t)(uint32_t)fd) |
                      ((uint64_t)(uint32_t)++fds->value << 32);

        int op = emask && cache != fds->cache
                 ? EPOLL_CTL_MOD
                 : EPOLL_CTL_ADD;

        if (__evio_likely(!epoll_ctl(loop->fd, op, fd, &ev)))
            continue;

        switch (errno) {
            case EEXIST:
                if (cache == fds->cache) {
                    --fds->value;
                    continue;
                }
                if (__evio_likely(!epoll_ctl(loop->fd, EPOLL_CTL_MOD, fd, &ev)))
                    continue;
                break;

            case ENOENT:
                if (__evio_likely(!epoll_ctl(loop->fd, EPOLL_CTL_ADD, fd, &ev)))
                    continue;
                break;

            case EPERM:
                if (!(cache & EVIO_FLAG)) {
                    loop->fderrors = (int *)evio_array_resize(
                                         loop->fderrors, sizeof(int),
                                         ++loop->fderrors_count,
                                         &loop->fderrors_total);
                    loop->fderrors[loop->fderrors_count - 1] = fd;
                }

                fds->cache = EVIO_FLAG;
                continue;
        }

        evio_queue_fd_errors(loop, fd);
        --fds->value;
    } while (loop->fdchanges_count);
}

static __evio_noinline __evio_nonnull(1)
void evio_timer_update(evio_loop *loop)
{
    if (!loop->timer_count || loop->timer[EVIO_HROOT].time >= loop->time)
        return;

    do {
        evio_timer *w = loop->timer[EVIO_HROOT].w;

        if (w->repeat) {
            w->time += w->repeat;
            if (w->time < loop->time)
                w->time = loop->time;

            loop->timer[EVIO_HROOT].time = w->time;
            evio_heap_down(loop->timer, EVIO_HROOT, loop->timer_count);
        } else {
            evio_timer_stop(loop, w);
        }

        evio_queue_event(loop, &w->base, EVIO_TIMER);
    } while (
        loop->timer_count &&
        loop->timer[EVIO_HROOT].time < loop->time
    );
}

static __evio_noinline __evio_nonnull(1)
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
            fds->cache = 0;
            evio_queue_fd_change(loop, fd, EVIO_POLL);
        }
    }

    if (loop->reinit > 1 && loop->event.active) {
        evio_ref(loop);
        evio_poll_stop(loop, &loop->event);

        evio_eventfd_init(loop);
        evio_queue_event(loop, &loop->event.base, EVIO_POLL);
    }

    loop->reinit = 0;
}

static __evio_noinline __evio_nonnull(1)
void evio_poll_wait(evio_loop *loop, int timeout)
{
    if (__evio_unlikely(loop->fderrors_count))
        timeout = 0;

    int events_count = epoll_pwait(loop->fd,
                                   loop->events,
                                   loop->maxevents,
                                   timeout,
                                   &loop->epoll_sigmask);
    if (__evio_unlikely(events_count < 0)) {
        if (__evio_unlikely(errno != EINTR))
            abort();
        return;
    }

    for (size_t i = events_count; i--;) {
        struct epoll_event *ev = &loop->events[i];

        int fd = (uint32_t)ev->data.u64;
        evio_fds *fds = &loop->fds[fd];

        if (__evio_unlikely((uint32_t)fds->value != (uint32_t)(ev->data.u64 >> 32))) {
            loop->reinit = 1;
            continue;
        }

        uint8_t emask = (ev->events & (EPOLLIN  | EPOLLERR | EPOLLHUP) ? EVIO_READ  : 0) |
                        (ev->events & (EPOLLOUT | EPOLLERR | EPOLLHUP) ? EVIO_WRITE : 0);

        if (__evio_unlikely(emask & ~fds->emask)) {
            fds->cache = fds->emask;

            ev->events = ((fds->emask & EVIO_READ)  ? EPOLLIN  : 0) |
                         ((fds->emask & EVIO_WRITE) ? EPOLLOUT : 0);

            int op = fds->emask ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;

            if (__evio_unlikely(epoll_ctl(loop->fd, op, fd, ev))) {
                loop->reinit = 1;
                continue;
            }
        }

        if (__evio_likely(!fds->flags))
            evio_queue_fd_events(loop, fd, emask);
    }

    if (__evio_unlikely(events_count == loop->maxevents &&
                        events_count < EVIO_MAX_EVENTS)) {
        loop->events = (struct epoll_event *)evio_array_resize(
                           loop->events, sizeof(struct epoll_event),
                           (size_t)events_count + 1, &loop->events_total);
        loop->maxevents = loop->events_total < EVIO_MAX_EVENTS ?
                          loop->events_total : EVIO_MAX_EVENTS;
    }

    for (size_t i = loop->fderrors_count; i--;) {
        int fd = loop->fderrors[i];
        evio_fds *fds = &loop->fds[fd];

        if ((fds->cache & EVIO_FLAG) && fds->emask) {
            if (__evio_likely(!fds->flags))
                evio_queue_fd_events(loop, fd, fds->emask);
        } else {
            loop->fderrors[i] = loop->fderrors[--loop->fderrors_count];
            fds->cache = 0;
        }
    }
}

void evio_invoke_pending(evio_loop *loop)
{
    while (loop->pending_count) {
        evio_pending *p = (evio_pending *)loop->pending.data + --loop->pending_count;
        p->base->pending = 0;
        p->base->cb(loop, p->base, p->emask);
    }
}

void evio_clear_pending(evio_loop *loop, evio_base *base)
{
    if (__evio_unlikely(base->pending)) {
        evio_pending *p = (evio_pending *)loop->pending.data + base->pending - 1;
        p->base = &loop->pending;
        base->pending = 0;
    }
}

size_t evio_pending_count(evio_loop *loop)
{
    return loop->pending_count;
}

static __evio_inline __evio_nonnull(1) __evio_nodiscard
uint64_t evio_gettime(evio_loop *loop)
{
    struct timespec ts;
    if (__evio_unlikely(clock_gettime(loop->clock_id, &ts)))
        abort();

    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)ts.tv_nsec / 1000000ULL;
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

    struct timespec ts;
    if (!clock_getres(CLOCK_MONOTONIC_COARSE, &ts) && ts.tv_nsec <= 1000000) {
        loop->clock_id = CLOCK_MONOTONIC_COARSE;
    } else {
        loop->clock_id = CLOCK_MONOTONIC;
    }

    loop->time = evio_gettime(loop);

    evio_init(&loop->pending, evio_dummy_cb);
    loop->pending.data = NULL;

    evio_init(&loop->event.base, evio_eventfd_cb);
    loop->event.data = NULL;
    loop->event.fd = -1;

    loop->maxevents = maxevents > 0 &&
                      maxevents <= EVIO_MAX_EVENTS ?
                      maxevents  : EVIO_DEF_EVENTS;
    loop->events = (struct epoll_event *)evio_array_resize(
                       loop->events, sizeof(struct epoll_event),
                       loop->maxevents, &loop->events_total);

    sigemptyset(&loop->epoll_sigmask);
    sigaddset(&loop->epoll_sigmask, SIGPROF);
    return loop;
}

void evio_loop_free(evio_loop *loop)
{
    loop->pending_count = 0;

    if (loop->cleanup_count) {
        evio_queue_events(loop, (evio_base **)loop->cleanup,
                          loop->cleanup_count, EVIO_CLEANUP);
        evio_invoke_pending(loop);
    }

    for (int i = NSIG - 1; i--;) {
        evio_sig *sig = &signals[i];

        if (atomic_load_explicit(&sig->loop, memory_order_acquire) == loop) {
            sig->head = NULL;

            if (__evio_unlikely(sigaction(i + 1, &sig->sa_old, NULL)))
                abort();

            atomic_store_explicit(&sig->loop, NULL, memory_order_release);
        }
    }

    if (loop->event.fd >= 0) {
        close(loop->event.fd);
        loop->event.fd = -1;
    }

    if (loop->fd >= 0) {
        close(loop->fd);
        loop->fd = -1;
    }

    evio_free(loop->pending.data);
    evio_free(loop->fds);
    evio_free(loop->fdchanges);
    evio_free(loop->fderrors);
    evio_free(loop->timer);
    evio_free(loop->idle);
    evio_free(loop->async);
    evio_free(loop->prepare);
    evio_free(loop->check);
    evio_free(loop->cleanup);
    evio_free(loop->events);
    evio_free(loop);
}

void evio_loop_fork(evio_loop *loop)
{
    loop->reinit = 2;
}

uint64_t evio_time(evio_loop *loop)
{
    return loop->time;
}

void evio_update_time(evio_loop *loop)
{
    loop->time = evio_gettime(loop);
}

void evio_ref(evio_loop *loop)
{
    if (__evio_unlikely(++loop->refcount == 0))
        abort();
}

void evio_unref(evio_loop *loop)
{
    if (__evio_unlikely(loop->refcount-- == 0))
        abort();
}

size_t evio_refcount(evio_loop *loop)
{
    return loop->refcount;
}

void evio_set_userdata(evio_loop *loop, void *data)
{
    loop->data = data;
}

void *evio_get_userdata(evio_loop *loop)
{
    return loop->data;
}

static __evio_inline __evio_nonnull(1) __evio_nodiscard
int evio_timeout(evio_loop *loop)
{
    if (!loop->refcount || loop->idle_count)
        return 0;

    if (atomic_load_explicit(&loop->event_pending, memory_order_acquire))
        return 0;

    if (!loop->timer_count)
        return -1;

    evio_node *node = &loop->timer[EVIO_HROOT];
    if (node->time <= loop->time)
        return 0;

    uint64_t diff = node->time - loop->time;
    if (__evio_unlikely(diff > INT_MAX))
        return INT_MAX;

    return diff;
}

int evio_run(evio_loop *loop, uint8_t flags)
{
    flags &= EVIO_RUN_NOWAIT | EVIO_RUN_ONCE;
    loop->done = EVIO_BREAK_CANCEL;
    evio_invoke_pending(loop);

    do {
        if (loop->prepare_count) {
            evio_queue_events(loop, (evio_base **)loop->prepare,
                              loop->prepare_count, EVIO_PREPARE);
            evio_invoke_pending(loop);
        }

        if (__evio_unlikely(loop->done))
            break;

        evio_loop_reinit(loop);
        evio_poll_update(loop);
        loop->time = evio_gettime(loop);

        atomic_store_explicit(&loop->eventfd_allow, 1, memory_order_seq_cst);
        evio_poll_wait(loop, (flags & EVIO_RUN_NOWAIT) ? 0 : evio_timeout(loop));
        atomic_store_explicit(&loop->eventfd_allow, 0, memory_order_relaxed);

        if (atomic_load_explicit(&loop->event_pending, memory_order_acquire))
            evio_queue_event(loop, &loop->event.base, EVIO_POLL);

        loop->time = evio_gettime(loop);
        evio_timer_update(loop);

        if (loop->idle_count && !loop->pending_count) {
            evio_queue_events(loop, (evio_base **)loop->idle,
                              loop->idle_count, EVIO_IDLE);
        }

        evio_invoke_pending(loop);

        if (loop->check_count) {
            evio_queue_events(loop, (evio_base **)loop->check,
                              loop->check_count, EVIO_CHECK);
            evio_invoke_pending(loop);
        }
    } while (__evio_likely(
                 loop->refcount &&
                 loop->done == EVIO_BREAK_CANCEL &&
                 flags == EVIO_RUN_DEFAULT
             ));

    if (loop->done == EVIO_BREAK_ONE)
        loop->done = EVIO_BREAK_CANCEL;

    return loop->refcount ||
           loop->pending_count;
}

void evio_break(evio_loop *loop, uint8_t state)
{
    loop->done = state & (EVIO_BREAK_ONE | EVIO_BREAK_ALL);
}

void evio_walk(evio_loop *loop, evio_cb cb, uint16_t emask)
{
    if (emask & (EVIO_POLL | EVIO_READ | EVIO_WRITE)) {
        for (int fd = 0; fd <= loop->maxfd; ++fd) {
            evio_fds *fds = &loop->fds[fd];

            for (evio_poll *w = (evio_poll *)fds->head; w; w = (evio_poll *)w->next) {
                if (__evio_unlikely(w->cb == evio_eventfd_cb))
                    continue;

                if (emask & (EVIO_POLL | w->emask))
                    cb(loop, &w->base, EVIO_WALK | EVIO_POLL | w->emask);
            }
        }
    }

    if (emask & EVIO_TIMER) {
        for (size_t i = loop->timer_count + EVIO_HROOT; i-- > EVIO_HROOT;) {
            evio_timer *w = loop->timer[i].w;
            cb(loop, &w->base, EVIO_WALK | EVIO_TIMER);
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
    if (__evio_likely(base->active))
        evio_queue_event(loop, base, emask);
}

void evio_feed_fd_event(evio_loop *loop, int fd, uint8_t emask)
{
    if (__evio_likely(fd >= 0 && fd <= loop->maxfd))
        evio_queue_fd_events(loop, fd, emask);
}

void evio_feed_fd_error(evio_loop *loop, int fd)
{
    if (__evio_likely(fd >= 0 && fd <= loop->maxfd))
        evio_queue_fd_errors(loop, fd);
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

    if (!atomic_exchange_explicit(&loop->signal_pending, 1, memory_order_acq_rel))
        evio_eventfd_write(loop);
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
        evio_queue_event(loop, &w->base, EVIO_SIGNAL);
}

void evio_feed_signal_error(evio_loop *loop, int signum)
{
    if (__evio_unlikely(signum <= 0 || signum >= NSIG))
        return;

    evio_sig *sig = &signals[signum - 1];

    evio_loop *ptr = atomic_load_explicit(&sig->loop, memory_order_acquire);
    if (__evio_unlikely(!ptr || ptr != loop))
        return;

    atomic_store_explicit(&sig->pending, 0, memory_order_release);

    while (sig->head) {
        evio_signal *w = (evio_signal *)sig->head;
        evio_signal_stop(loop, w);
        evio_queue_event(loop, &w->base, EVIO_SIGNAL | EVIO_ERROR);
    }
}

void evio_poll_start(evio_loop *loop, evio_poll *w)
{
    if (__evio_unlikely(w->active))
        return;

    w->active = 1;
    evio_ref(loop);

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

    evio_queue_fd_change(loop, w->fd, w->emask & EVIO_POLL);
    w->emask &= ~EVIO_POLL;
}

void evio_poll_stop(evio_loop *loop, evio_poll *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    evio_fds *fds = &loop->fds[w->fd];
    evio_list_remove(&fds->head, &w->list);

    evio_unref(loop);
    w->active = 0;

    evio_queue_fd_change(loop, w->fd, 0);
}

void evio_timer_start(evio_loop *loop, evio_timer *w)
{
    if (__evio_unlikely(w->active))
        return;

    uint64_t time = w->time + loop->time;
    if (__evio_unlikely(time < w->time)) {
        w->time = UINT64_MAX;
    } else {
        w->time = time;
    }

    w->active = ++loop->timer_count + EVIO_HROOT - 1;
    evio_ref(loop);

    loop->timer = (evio_node *)evio_array_resize(
                      loop->timer, sizeof(evio_node),
                      w->active + 1, &loop->timer_total);

    evio_node *node = &loop->timer[w->active];
    node->w = w;
    node->time = w->time;

    evio_heap_up(loop->timer, w->active);
}

void evio_timer_stop(evio_loop *loop, evio_timer *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    size_t count = --loop->timer_count;

    if (__evio_likely(w->active < count + EVIO_HROOT)) {
        loop->timer[w->active] = loop->timer[count + EVIO_HROOT];
        evio_heap_adjust(loop->timer, w->active, count);
    }

    if (__evio_likely(w->time > loop->time)) {
        w->time -= loop->time;
    } else {
        w->time = 0;
    }

    evio_unref(loop);
    w->active = 0;
}

void evio_timer_again(evio_loop *loop, evio_timer *w)
{
    evio_clear_pending(loop, &w->base);

    if (w->active) {
        if (w->repeat) {
            w->time = loop->time + w->repeat;
            if (__evio_unlikely(w->time < loop->time))
                w->time = UINT64_MAX;

            loop->timer[w->active].time = w->time;
            evio_heap_adjust(loop->timer, w->active, loop->timer_count);
        } else {
            evio_timer_stop(loop, w);
        }
    } else if (w->repeat) {
        w->time = w->repeat;
        evio_timer_start(loop, w);
    }
}

uint64_t evio_timer_remaining(evio_loop *loop, evio_timer *w)
{
    if (!w->active || w->time < loop->time)
        return 0;

    return w->time - loop->time;
}

void evio_signal_start(evio_loop *loop, evio_signal *w)
{
    if (__evio_unlikely(w->active))
        return;

    evio_sig *sig = &signals[w->signum - 1];

    evio_loop *ptr = atomic_exchange_explicit(&sig->loop, loop, memory_order_acq_rel);
    if (__evio_unlikely(ptr && ptr != loop))
        abort();

    evio_list_insert(&sig->head, &w->list);

    w->active = 1;
    evio_ref(loop);

    if (!w->next) {
        evio_eventfd_init(loop);

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = evio_feed_signal;
        sigfillset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;

        if (__evio_unlikely(sigaction(w->signum, &sa, &sig->sa_old)))
            abort();
    }
}

void evio_signal_stop(evio_loop *loop, evio_signal *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active))
        return;

    evio_sig *sig = &signals[w->signum - 1];
    evio_list_remove(&sig->head, &w->list);

    if (!sig->head) {
        if (__evio_unlikely(sigaction(w->signum, &sig->sa_old, NULL)))
            abort();

        atomic_store_explicit(&sig->loop, NULL, memory_order_release);
    }

    evio_unref(loop);
    w->active = 0;
}

void evio_async_start(evio_loop *loop, evio_async *w)
{
    if (__evio_unlikely(w->active))
        return;

    evio_eventfd_init(loop);
    atomic_store_explicit(&w->status, 0, memory_order_release);

    w->active = ++loop->async_count;
    evio_ref(loop);

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

    evio_unref(loop);
    w->active = 0;
}

void evio_async_send(evio_loop *loop, evio_async *w)
{
    atomic_store_explicit(&w->status, 1, memory_order_release);

    if (!atomic_exchange_explicit(&loop->async_pending, 1, memory_order_acq_rel))
        evio_eventfd_write(loop);
}

void evio_idle_start(evio_loop *loop, evio_idle *w)
{
    if (__evio_unlikely(w->active))
        return;

    w->active = ++loop->idle_count;
    evio_ref(loop);

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

    evio_unref(loop);
    w->active = 0;
}

void evio_prepare_start(evio_loop *loop, evio_prepare *w)
{
    if (__evio_unlikely(w->active))
        return;

    w->active = ++loop->prepare_count;
    evio_ref(loop);

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

    evio_unref(loop);
    w->active = 0;
}

void evio_check_start(evio_loop *loop, evio_check *w)
{
    if (__evio_unlikely(w->active))
        return;

    w->active = ++loop->check_count;
    evio_ref(loop);

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

    evio_unref(loop);
    w->active = 0;
}

void evio_cleanup_start(evio_loop *loop, evio_cleanup *w)
{
    if (__evio_unlikely(w->active))
        return;

    w->active = ++loop->cleanup_count;

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

    w->active = 0;
}
