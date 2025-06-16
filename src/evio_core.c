#include <errno.h>
#include <sys/epoll.h>

#include "evio_core.h"

void evio_queue_event(evio_loop *loop, evio_base *base, evio_mask emask)
{
    if (__evio_unlikely(base->pending)) {
        evio_pending *p = &loop->pending.ptr[base->pending - 1];
        p->emask |= emask;
        return;
    }

    base->pending = ++loop->pending.count;
    loop->pending.ptr = evio_list_resize(loop->pending.ptr, sizeof(evio_pending),
                                         loop->pending.count, &loop->pending.total);

    evio_pending *p = &loop->pending.ptr[base->pending - 1];
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
        evio_poll *w = (evio_poll *)(fds->list.ptr[i]);
        if (w->emask & emask) {
            evio_queue_event(loop, &w->base, EVIO_POLL | (w->emask & emask));
        }
    }
}

void evio_queue_fd_errors(evio_loop *loop, int fd)
{
    EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);
    evio_fds *fds = &loop->fds.ptr[fd];

    for (size_t i = fds->list.count; i--;) {
        evio_poll *w = (evio_poll *)(fds->list.ptr[i]);
        evio_poll_stop(loop, w);
        evio_queue_event(loop, &w->base, EVIO_POLL | EVIO_READ | EVIO_WRITE | EVIO_ERROR);
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

void evio_queue_fd_change(evio_loop *loop, int fd, uint8_t flags)
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

    if (loop->fdchanges.count-- <= 1) {
        loop->fdchanges.count = 0;
        return;
    }

    int fd = loop->fdchanges.ptr[loop->fdchanges.count];
    EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);

    evio_fds *fds = &loop->fds.ptr[fd];
    EVIO_ASSERT(fds->changes == loop->fdchanges.count + 1);

    fds->changes = idx + 1;
    loop->fdchanges.ptr[idx] = fd;
}

void evio_flush_fd_error(evio_loop *loop, int idx)
{
    EVIO_ASSERT(idx >= 0 && (size_t)idx < loop->fderrors.count);

    if (loop->fderrors.count-- <= 1) {
        loop->fderrors.count = 0;
        return;
    }

    int fd = loop->fderrors.ptr[loop->fderrors.count];
    EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);

    evio_fds *fds = &loop->fds.ptr[fd];
    EVIO_ASSERT(fds->errors == loop->fderrors.count + 1);

    fds->errors = idx + 1;
    loop->fderrors.ptr[idx] = fd;
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
    fds->cache = 0;
    fds->flags = EVIO_FD_INVAL;

    if (__evio_likely(!epoll_ctl(loop->fd, EPOLL_CTL_DEL, fd, NULL))) {
        return 0;
    }

    return errno == EPERM ? 0 : -1;
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
    while (loop->pending.count) {
        evio_pending *p = &loop->pending.ptr[--loop->pending.count];
        p->base->pending = 0;
        p->base->cb(loop, p->base, p->emask);
    }
}

void evio_clear_pending(evio_loop *loop, evio_base *base)
{
    if (__evio_unlikely(base->pending)) {
        loop->pending.ptr[base->pending - 1] = loop->pending.ptr[--loop->pending.count];
        loop->pending.ptr[base->pending - 1].base->pending = base->pending;
        base->pending = 0;
    }
}

size_t evio_pending_count(const evio_loop *loop)
{
    return loop->pending.count;
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
