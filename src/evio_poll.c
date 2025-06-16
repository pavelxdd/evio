#include <errno.h>
#include <sys/epoll.h>

#include "evio_core.h"
#include "evio_poll.h"

void evio_poll_start(evio_loop *loop, evio_poll *w)
{
    EVIO_ASSERT(w->fd >= 0);

    if (__evio_unlikely(w->active)) {
        return;
    }

    loop->fds.ptr = evio_list_resize(loop->fds.ptr, sizeof(*loop->fds.ptr),
                                     (size_t)w->fd + 1, &loop->fds.total);

    if ((size_t)w->fd >= loop->fds.count) {
        memset(&loop->fds.ptr[loop->fds.count], 0,
               (loop->fds.total - loop->fds.count) * sizeof(*loop->fds.ptr));
        loop->fds.count = (size_t)w->fd + 1;
    }

    evio_fds *fds = &loop->fds.ptr[w->fd];

    w->active = ++fds->list.count;
    evio_ref(loop);

    fds->list.ptr = evio_list_resize(fds->list.ptr, sizeof(*fds->list.ptr),
                                     fds->list.count, &fds->list.total);
    fds->list.ptr[w->active - 1] = &w->base;

    evio_queue_fd_change(loop, w->fd, w->emask & EVIO_POLL);
    w->emask &= ~EVIO_POLL;
}

void evio_poll_stop(evio_loop *loop, evio_poll *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active)) {
        return;
    }

    EVIO_ASSERT(w->fd >= 0 && (size_t)w->fd < loop->fds.count);

    evio_fds *fds = &loop->fds.ptr[w->fd];

    fds->list.ptr[w->active - 1] = fds->list.ptr[--fds->list.count];
    fds->list.ptr[w->active - 1]->active = w->active;

    evio_unref(loop);
    w->active = 0;

    if (evio_invalidate_fd(loop, w->fd) > 0) {
        evio_queue_fd_change(loop, w->fd, 0);
    }
}

void evio_poll_change(evio_loop *loop, evio_poll *w, int fd, evio_mask emask)
{
    emask &= EVIO_READ | EVIO_WRITE;

    if (fd != w->fd) {
        evio_poll_stop(loop, w);
        evio_poll_set(w, fd, emask);

        if (emask) {
            evio_poll_start(loop, w);
        }
        return;
    }

    if (!emask) {
        evio_poll_stop(loop, w);
        w->emask = 0;
        return;
    }

    if (!w->active) {
        w->emask = emask | EVIO_POLL;
        evio_poll_start(loop, w);
        return;
    }

    EVIO_ASSERT(w->fd >= 0 && (size_t)w->fd < loop->fds.count);

    if (w->emask != emask) {
        w->emask = emask;
        evio_clear_pending(loop, &w->base);
        evio_queue_fd_change(loop, w->fd, EVIO_POLL);
    }
}

void evio_poll_update(evio_loop *loop)
{
    struct epoll_event ev = { 0 };

    while (loop->fdchanges.count) {
        int fd = loop->fdchanges.ptr[--loop->fdchanges.count];
        EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);

        evio_fds *fds = &loop->fds.ptr[fd];
        EVIO_ASSERT(fds->changes == loop->fdchanges.count + 1);

        uint8_t flags = fds->flags;
        uint8_t emask = fds->emask;

        fds->changes = 0;
        fds->flags = 0;
        fds->emask = 0;

        for (size_t i = fds->list.count; i--;) {
            const evio_poll *w = (evio_poll *)(fds->list.ptr[i]);
            fds->emask |= w->emask;
        }

        fds->emask &= EVIO_POLLET | EVIO_READ | EVIO_WRITE;

        if (!fds->emask) {
            continue;
        }

        if (fds->emask == emask && !(flags & EVIO_POLL)) {
            continue;
        }

        uint8_t cache = fds->cache;
        fds->cache = fds->emask;

        ev.events = ((fds->emask & EVIO_READ)  ? EPOLLIN  : 0) |
                    ((fds->emask & EVIO_WRITE) ? EPOLLOUT : 0) |
                    ((fds->emask & EVIO_POLLET) ? EPOLLET  : 0);

        ev.data.u64 = ((uint64_t)fd) | ((uint64_t)++fds->gen << 32);

        int op = emask && cache != fds->cache ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

        if (loop->iou) {
            evio_uring_ctl(loop, op, fd, &ev);
            continue;
        }

        if (__evio_likely(!epoll_ctl(loop->fd, op, fd, &ev))) {
            continue;
        }

        switch (errno) {
            case EEXIST:
                if (cache == fds->cache) {
                    --fds->gen;
                    continue;
                }
                if (__evio_likely(!epoll_ctl(loop->fd, EPOLL_CTL_MOD, fd, &ev))) {
                    continue;
                }
                break;

            case ENOENT:
                if (__evio_likely(!epoll_ctl(loop->fd, EPOLL_CTL_ADD, fd, &ev))) {
                    continue;
                }
                break;

            case EPERM:
                evio_queue_fd_error(loop, fd);
                continue;
        }

        evio_queue_fd_errors(loop, fd);
        --fds->gen;
    }

    if (loop->iou) {
        evio_uring_flush(loop);
    }
}

void evio_poll_wait(evio_loop *loop, int timeout)
{
    EVIO_ASSERT(timeout >= -1);

    if (__evio_unlikely(loop->fderrors.count)) {
        timeout = 0;
    }

    int events_count;
    for (;;) {
        events_count = epoll_pwait(loop->fd,
                                   loop->events.ptr,
                                   loop->events.count,
                                   timeout, &loop->sigmask);
        if (__evio_likely(events_count >= 0)) {
            break;
        }

        int err = errno;
        if (err == EINTR) {
            continue;
        }

        EVIO_ABORT("epoll_pwait() failed, error %d: %s\n", err, EVIO_STRERROR(err));
    }

    for (size_t i = events_count; i--;) {
        struct epoll_event *ev = &loop->events.ptr[i];

        uint32_t fd32 = ev->data.u64 & UINT32_MAX;
        if (__evio_unlikely(fd32 >= loop->fds.count)) {
            EVIO_ABORT("Invalid fd %u\n", fd32);
        }

        int fd = fd32;

        evio_fds *fds = &loop->fds.ptr[fd];
        if (__evio_unlikely(fds->gen != (ev->data.u64 >> 32))) {
            continue;
        }

        if (!evio_invalidate_fd(loop, fd)) {
            continue;
        }

        uint8_t emask = ((ev->events & (EPOLLIN  | EPOLLERR | EPOLLHUP)) ? EVIO_READ   : 0) |
                        ((ev->events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) ? EVIO_WRITE  : 0) |
                        ((ev->events & (EPOLLET))                        ? EVIO_POLLET : 0);

        if (__evio_unlikely(emask & ~fds->emask)) {
            fds->cache = fds->emask;

            ev->events = ((fds->emask & EVIO_READ)  ? EPOLLIN  : 0) |
                         ((fds->emask & EVIO_WRITE) ? EPOLLOUT : 0) |
                         ((fds->emask & EVIO_POLLET) ? EPOLLET  : 0);

            int op = fds->emask ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;

            if (!loop->iou || op == EPOLL_CTL_DEL) {
                if (__evio_unlikely(epoll_ctl(loop->fd, op, fd, ev))) {
                    int err = errno;
                    EVIO_ABORT("epoll_ctl() failed, error %d: %s\n", err, EVIO_STRERROR(err));
                }
            } else {
                evio_uring_ctl(loop, op, fd, ev);
            }
        }

        if (__evio_likely(!fds->changes)) {
            evio_queue_fd_events(loop, fd, emask);
        }
    }

    if (loop->iou) {
        evio_uring_flush(loop);
    }

    if (__evio_unlikely((size_t)events_count == loop->events.count &&
                        (size_t)events_count < EVIO_MAX_EVENTS)) {
        loop->events.ptr = evio_list_resize(loop->events.ptr, sizeof(*loop->events.ptr),
                                            (size_t)events_count + 1, &loop->events.total);
        loop->events.count = loop->events.total < EVIO_MAX_EVENTS ?
                             loop->events.total : EVIO_MAX_EVENTS;
    }

    for (size_t i = loop->fderrors.count; i--;) {
        int fd = loop->fderrors.ptr[i];
        EVIO_ASSERT(fd >= 0 && (size_t)fd < loop->fds.count);

        evio_fds *fds = &loop->fds.ptr[fd];
        EVIO_ASSERT(fds->errors == i + 1);

        if (fds->emask) {
            if (__evio_likely(!fds->changes)) {
                evio_queue_fd_events(loop, fd, fds->emask);
            }
        } else {
            evio_flush_fd_error(loop, i);
            fds->errors = 0;
            fds->cache = 0;
        }
    }
}
