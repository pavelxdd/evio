#include <errno.h>
#include <sys/epoll.h>

#include "evio_core.h"

/**
 * @brief Sets the pending state of a watcher.
 * @details This encodes the pending array index and queue index into the
 * `base->pending` field, marking it as active in the pending queue.
 * @param base The watcher's base structure.
 * @param index The 0-based index in the pending array.
 * @param queue The queue index (0 or 1).
 */
static inline __evio_nonnull(1)
void evio_pending_set(evio_base *base, size_t index, size_t queue)
{
    base->pending = (index << 1) + 1 + queue;
}

/**
 * @brief Gets the pending array index from a watcher's pending state.
 * @param base The watcher's base structure.
 * @return The 0-based index in the pending array.
 */
static inline __evio_nonnull(1) __evio_nodiscard
size_t evio_pending_get_index(const evio_base *base)
{
    return (base->pending - 1) >> 1;
}

/**
 * @brief Gets the pending queue index from a watcher's pending state.
 * @param base The watcher's base structure.
 * @return The queue index (0 or 1).
 */
static inline __evio_nonnull(1) __evio_nodiscard
size_t evio_pending_get_queue(const evio_base *base)
{
    return (base->pending - 1) & 1;
}

void evio_queue_event(evio_loop *loop, evio_base *base, evio_mask emask)
{
    if (__evio_unlikely(base->pending)) {
        const size_t queue = evio_pending_get_queue(base);
        const size_t index = evio_pending_get_index(base);

        evio_pending_list *pending = &loop->pending[queue];
        EVIO_ASSERT(pending->count > index);
        EVIO_ASSERT(pending->ptr[index].base == base);

        evio_pending *p = &pending->ptr[index];
        p->emask |= emask;
        return;
    }

    const size_t queue = loop->pending_queue;
    evio_pending_list *pending = &loop->pending[queue];

    const size_t index = pending->count++;
    evio_pending_set(base, index, queue);

    pending->ptr = evio_list_resize(pending->ptr, sizeof(evio_pending),
                                    pending->count, &pending->total);

    evio_pending *p = &pending->ptr[index];
    p->base = base;
    p->emask = emask;
}

void evio_queue_events(evio_loop *loop, evio_base **base, size_t count, evio_mask emask)
{
    for (size_t i = count; i--;) {
        evio_queue_event(loop, base[i], emask);
    }
}

void evio_queue_fd_events(evio_loop *loop, int fd, evio_mask emask)
{
    EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);

    evio_fds *fds = &loop->fds.ptr[fd];

    for (size_t i = fds->list.count; i--;) {
        evio_poll *w = container_of(fds->list.ptr[i], evio_poll, base);
        if (w->emask & emask) {
            evio_queue_event(loop, &w->base, EVIO_POLL | (w->emask & emask));
        }
    }
}

void evio_queue_fd_errors(evio_loop *loop, int fd)
{
    EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);

    evio_fds *fds = &loop->fds.ptr[fd];

    while (fds->list.count > 0) {
        evio_base *base = fds->list.ptr[fds->list.count - 1];
        evio_poll *w = container_of(base, evio_poll, base);

        EVIO_ASSERT(w->active);

        evio_clear_pending(loop, base);

        fds->list.count--;

        evio_unref(loop);
        w->active = 0;

        evio_queue_event(loop, base, EVIO_POLL | EVIO_READ | EVIO_WRITE | EVIO_ERROR);
    }
}

void evio_queue_fd_error(evio_loop *loop, int fd)
{
    EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);

    evio_fds *fds = &loop->fds.ptr[fd];

    if (!fds->errors) {
        fds->errors = ++loop->fderrors.count;
        loop->fderrors.ptr = evio_list_resize(loop->fderrors.ptr, sizeof(*loop->fderrors.ptr),
                                              loop->fderrors.count, &loop->fderrors.total);
        loop->fderrors.ptr[fds->errors - 1] = fd;
    }
}

void evio_queue_fd_change(evio_loop *loop, int fd, evio_flag flags)
{
    EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);

    evio_fds *fds = &loop->fds.ptr[fd];

    if (__evio_likely(!fds->changes)) {
        fds->changes = ++loop->fdchanges.count;
        loop->fdchanges.ptr = evio_list_resize(loop->fdchanges.ptr, sizeof(*loop->fdchanges.ptr),
                                               loop->fdchanges.count, &loop->fdchanges.total);
        loop->fdchanges.ptr[fds->changes - 1] = fd;
    }

    fds->flags &= ~EVIO_FD_INVAL;
    fds->flags |= flags;
}

void evio_flush_fd_change(evio_loop *loop, int idx)
{
    EVIO_ASSERT(idx >= 0 && (size_t)idx < loop->fdchanges.count);

    if (loop->fdchanges.count < 2) {
        loop->fdchanges.count = 0;
        return;
    }

    int fd = loop->fdchanges.ptr[loop->fdchanges.count - 1];
    EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);

    evio_fds *fds = &loop->fds.ptr[fd];
    EVIO_ASSERT(fds->changes == loop->fdchanges.count);

    fds->changes = idx + 1;
    loop->fdchanges.ptr[idx] = fd;
    loop->fdchanges.count--;
}

void evio_flush_fd_error(evio_loop *loop, int idx)
{
    EVIO_ASSERT(idx >= 0 && (size_t)idx < loop->fderrors.count);

    if (loop->fderrors.count < 2) {
        loop->fderrors.count = 0;
        return;
    }

    int fd = loop->fderrors.ptr[loop->fderrors.count - 1];
    EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);

    evio_fds *fds = &loop->fds.ptr[fd];
    EVIO_ASSERT(fds->errors == loop->fderrors.count);

    fds->errors = idx + 1;
    loop->fderrors.ptr[idx] = fd;
    loop->fderrors.count--;
}

int evio_invalidate_fd(evio_loop *loop, int fd)
{
    EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);

    evio_fds *fds = &loop->fds.ptr[fd];

    if (fds->list.count) {
        return 1;
    }

    if (fds->flags & EVIO_FD_INVAL) {
        return 1;
    }

    if (fds->changes) {
        evio_flush_fd_change(loop, fds->changes - 1);
        fds->changes = 0;
    }

    if (fds->errors) {
        evio_flush_fd_error(loop, fds->errors - 1);
        fds->errors = 0;
    }

    fds->emask = 0;
    fds->flags = EVIO_FD_INVAL;

    if (__evio_likely(!epoll_ctl(loop->fd, EPOLL_CTL_DEL, fd, NULL))) {
        return 0;
    }

    int err = errno;
    if (err == EPERM || err == ENOENT) {
        return 0;
    }

    return -1;
}

void evio_feed_signal(evio_loop *loop, int signum)
{
    if (__evio_unlikely(signum <= 0 || signum >= NSIG)) {
        return;
    }

    evio_signal_queue_events(loop, signum);
}

void evio_invoke_pending(evio_loop *loop)
{
    for (;;) {
        const size_t queue = loop->pending_queue;
        evio_pending_list *pending = &loop->pending[queue];
        if (!pending->count) {
            break;
        }

        // Flip to the other queue.
        // Any new events queued by callbacks will go here.
        loop->pending_queue ^= 1;

        while (pending->count) {
            const size_t index = --pending->count;
            evio_pending *p = &pending->ptr[index];

            EVIO_ASSERT(evio_pending_get_queue(p->base) == queue);
            EVIO_ASSERT(evio_pending_get_index(p->base) == index);

            p->base->pending = 0;
            p->base->cb(loop, p->base, p->emask);
        }
    }
}

void evio_clear_pending(evio_loop *loop, evio_base *base)
{
    if (__evio_likely(!base->pending)) {
        return;
    }

    const size_t queue = evio_pending_get_queue(base);
    const size_t index = evio_pending_get_index(base);

    evio_pending_list *pending = &loop->pending[queue];

    EVIO_ASSERT(pending->count > index);
    EVIO_ASSERT(pending->ptr[index].base == base);

    pending->ptr[index] = pending->ptr[--pending->count];
    evio_pending_set(pending->ptr[index].base, index, queue);

    base->pending = 0;
}

size_t evio_pending_count(const evio_loop *loop)
{
    return loop->pending[0].count +
           loop->pending[1].count;
}

void evio_feed_event(evio_loop *loop, evio_base *base, evio_mask emask)
{
    if (__evio_likely(base->active)) {
        evio_queue_event(loop, base, emask);
    }
}

void evio_feed_fd_event(evio_loop *loop, int fd, evio_mask emask)
{
    if (__evio_likely(fd >= 0 && (size_t)fd < loop->fds.count)) {
        evio_queue_fd_events(loop, fd, emask);
    }
}

void evio_feed_fd_error(evio_loop *loop, int fd)
{
    if (__evio_likely(fd >= 0 && (size_t)fd < loop->fds.count)) {
        evio_queue_fd_errors(loop, fd);
    }
}
