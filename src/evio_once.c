#include "evio_core.h"
#include "evio_once.h"

/**
 * @brief Internal callback for the poll part of a once watcher.
 * @param loop The event loop.
 * @param base The base watcher pointer.
 * @param emask The received event mask.
 */
static void evio_once_poll_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_once *w = container_of(base, evio_once, io.base);
    evio_once_stop(loop, w);
    w->cb(loop, &w->base, EVIO_ONCE | emask);
}

/**
 * @brief Internal callback for the timer part of a once watcher.
 * @param loop The event loop.
 * @param base The base watcher pointer.
 * @param emask The received event mask.
 */
static void evio_once_timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_once *w = container_of(base, evio_once, tm.base);
    evio_once_stop(loop, w);
    w->cb(loop, &w->base, EVIO_ONCE | emask);
}

void evio_once_init(evio_once *w, evio_cb cb, int fd, evio_mask emask)
{
    evio_init(&w->base, cb);
    evio_poll_init(&w->io, evio_once_poll_cb, fd, emask);
    evio_timer_init(&w->tm, evio_once_timer_cb, 0);
}

void evio_once_start(evio_loop *loop, evio_once *w, evio_time after)
{
    if (__evio_unlikely(w->active)) {
        return;
    }

    // This takes one ref for the once watcher itself.
    evio_list_start(loop, &w->base, &loop->once, true);

    // Start the poll watcher, but cancel its ref since the once watcher holds it.
    evio_poll_start(loop, &w->io);
    evio_unref(loop);

    // Start the timer watcher, also canceling its ref.
    evio_timer_start(loop, &w->tm, after);
    evio_unref(loop);
}

void evio_once_stop(evio_loop *loop, evio_once *w)
{
    evio_clear_pending(loop, &w->base);
    evio_clear_pending(loop, &w->io.base);
    evio_clear_pending(loop, &w->tm.base);

    if (__evio_unlikely(!w->active)) {
        return;
    }

    // The ref/unref pairs here prevent the loop from exiting prematurely if
    // stopping one of the sub-watchers drops the refcount to zero before the
    // `once` watcher itself is stopped.
    evio_ref(loop);
    evio_poll_stop(loop, &w->io);

    evio_ref(loop);
    evio_timer_stop(loop, &w->tm);

    // This stops the once watcher and performs the final unref.
    evio_list_stop(loop, &w->base, &loop->once, true);
}
