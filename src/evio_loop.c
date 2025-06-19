#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "evio_core.h"
#include "evio_loop.h"

/**
 * @brief Gets the current monotonic time from the loop's configured clock.
 * @param loop The event loop.
 * @return The current time in nanoseconds.
 */
static __evio_nonnull(1) __evio_nodiscard
evio_time evio_clock_gettime(const evio_loop *loop)
{
    struct timespec ts;
    int rc = clock_gettime(loop->clock_id, &ts);
    if (__evio_unlikely(rc < 0)) {
        int err = errno;
        EVIO_ABORT("clock_gettime() failed, error %d: %s\n", err, EVIO_STRERROR(err));
    }

    return EVIO_TIME_FROM_SEC(ts.tv_sec) +
           EVIO_TIME(ts.tv_nsec);
}

/**
 * @brief Calculates the timeout for the `epoll_pwait` call.
 * @param loop The event loop.
 * @return The timeout in milliseconds, 0 for no-wait, or -1 for infinite wait.
 */
static int evio_timeout(evio_loop *loop)
{
    if (!loop->refcount || loop->idle.count) {
        return 0;
    }

    if (atomic_load_explicit(&loop->event_pending.value, memory_order_acquire)) {
        return 0;
    }

    if (!loop->timer.count) {
        return -1;
    }

    const evio_node *node = &loop->timer.ptr[0];
    if (node->time <= loop->time) {
        return 0;
    }

    const evio_time diff_ns = node->time - loop->time;
    const evio_time diff_ms = diff_ns / EVIO_TIME_PER_MSEC;

    if (__evio_unlikely(diff_ms >= INT_MAX)) {
        return INT_MAX;
    }

    return (int)diff_ms + !!(diff_ns % EVIO_TIME_PER_MSEC);
}

evio_loop *evio_loop_new(int flags)
{
    int fd = epoll_create1(EPOLL_CLOEXEC);
    if (__evio_unlikely(fd < 0)) {
        return NULL;
    }

    evio_loop *loop = evio_malloc(sizeof(*loop));
    *loop = (evio_loop) {
        .fd = fd,
        .event.cb = evio_eventfd_cb,
        .event.fd = -1,
    };

    if (flags & EVIO_FLAG_URING) {
        loop->iou = evio_uring_new();
    }

    // GCOVR_EXCL_START
    struct timespec ts;
    if (!clock_getres(CLOCK_MONOTONIC_COARSE, &ts) && ts.tv_nsec <= 1000000) {
        loop->clock_id = CLOCK_MONOTONIC_COARSE;
    } else {
        loop->clock_id = CLOCK_MONOTONIC;
    }
    // GCOVR_EXCL_STOP

    loop->time = evio_clock_gettime(loop);

    loop->events.count = EVIO_DEF_EVENTS;
    loop->events.ptr = evio_list_resize(loop->events.ptr, sizeof(*loop->events.ptr),
                                        loop->events.count, &loop->events.total);

    sigemptyset(&loop->sigmask);
    sigaddset(&loop->sigmask, SIGPROF);
    return loop;
}

void evio_loop_free(evio_loop *loop)
{
    loop->pending[0].count = 0;
    loop->pending[1].count = 0;

    if (loop->cleanup.count) {
        evio_queue_events(loop, loop->cleanup.ptr, loop->cleanup.count, EVIO_CLEANUP);
        evio_invoke_pending(loop);
    }

    evio_signal_cleanup_loop(loop);

    if (loop->iou) {
        evio_uring_free(loop->iou);
    }

    if (loop->event.fd >= 0) {
        close(loop->event.fd);
    }

    close(loop->fd);

    for (size_t i = loop->fds.count; i--;) {
        evio_fds *fds = &loop->fds.ptr[i];
        evio_free(fds->list.ptr);
    }

    evio_free(loop->pending[0].ptr);
    evio_free(loop->pending[1].ptr);
    evio_free(loop->fds.ptr);
    evio_free(loop->fdchanges.ptr);
    evio_free(loop->fderrors.ptr);
    evio_free(loop->timer.ptr);
    evio_free(loop->idle.ptr);
    evio_free(loop->async.ptr);
    evio_free(loop->prepare.ptr);
    evio_free(loop->check.ptr);
    evio_free(loop->cleanup.ptr);
    evio_free(loop->once.ptr);
    evio_free(loop->events.ptr);
    evio_free(loop);
}

evio_time evio_get_time(const evio_loop *loop)
{
    return loop->time;
}

void evio_update_time(evio_loop *loop)
{
    loop->time = evio_clock_gettime(loop);
}

void evio_ref(evio_loop *loop)
{
    if (__evio_unlikely(++loop->refcount == 0)) {
        EVIO_ABORT("Invalid loop (%p) refcount\n", (void *)loop);
    }
}

void evio_unref(evio_loop *loop)
{
    if (__evio_unlikely(loop->refcount-- == 0)) {
        EVIO_ABORT("Invalid loop (%p) refcount\n", (void *)loop);
    }
}

size_t evio_refcount(const evio_loop *loop)
{
    return loop->refcount;
}

void evio_set_userdata(evio_loop *loop, void *data)
{
    loop->data = data;
}

void *evio_get_userdata(const evio_loop *loop)
{
    return loop->data;
}

int evio_run(evio_loop *loop, int flags)
{
    int done = loop->done;
    if (done == EVIO_BREAK_ALL) {
        return 0;
    }

    flags &= EVIO_RUN_NOWAIT | EVIO_RUN_ONCE;
    loop->done = EVIO_BREAK_CANCEL;
    evio_invoke_pending(loop);

    do {
        if (loop->prepare.count) {
            evio_queue_events(loop, loop->prepare.ptr, loop->prepare.count, EVIO_PREPARE);
            evio_invoke_pending(loop);
        }

        if (__evio_unlikely(loop->done)) {
            break;
        }

        evio_poll_update(loop);
        loop->time = evio_clock_gettime(loop);

        atomic_store_explicit(&loop->eventfd_allow.value, 1, memory_order_seq_cst);
        evio_poll_wait(loop, (flags & EVIO_RUN_NOWAIT) ? 0 : evio_timeout(loop));
        atomic_store_explicit(&loop->eventfd_allow.value, 0, memory_order_relaxed);

        if (atomic_load_explicit(&loop->event_pending.value, memory_order_acquire)) {
            evio_queue_event(loop, &loop->event.base, EVIO_POLL);
        }

        loop->time = evio_clock_gettime(loop);
        evio_timer_update(loop);

        if (loop->idle.count && !loop->pending[loop->pending_queue].count) {
            evio_queue_events(loop, loop->idle.ptr, loop->idle.count, EVIO_IDLE);
        }

        evio_invoke_pending(loop);

        if (loop->check.count) {
            evio_queue_events(loop, loop->check.ptr, loop->check.count, EVIO_CHECK);
            evio_invoke_pending(loop);
        }
    } while (__evio_likely(
                 loop->refcount &&
                 loop->done == EVIO_BREAK_CANCEL &&
                 flags == EVIO_RUN_DEFAULT
             ));

    // GCOVR_EXCL_START
    EVIO_ASSERT(loop->pending[loop->pending_queue].count == 0);
    // GCOVR_EXCL_STOP

    if (loop->done == EVIO_BREAK_ALL) {
        return 0;
    }

    if (loop->done == EVIO_BREAK_ONE) {
        loop->done = done;
    }

    return loop->refcount;
}

void evio_break(evio_loop *loop, int state)
{
    loop->done = state & (EVIO_BREAK_ONE | EVIO_BREAK_ALL);
}

int evio_break_state(const evio_loop *loop)
{
    return loop->done;
}
