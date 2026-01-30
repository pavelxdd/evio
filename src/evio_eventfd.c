#include <errno.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "evio_core.h"
#include "evio_eventfd.h"
#include "evio_eventfd_sys.h"

void evio_eventfd_init(evio_loop *loop)
{
    if (__evio_likely(loop->event.active)) {
        return;
    }

    EVIO_ASSERT(loop->event.fd < 0);

    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        int err = errno;
        EVIO_ABORT("eventfd() failed, error %d: %s\n", err, EVIO_STRERROR(err));
    }

    loop->event.fd = fd;
    loop->event.emask = EVIO_POLL | EVIO_POLLET | EVIO_READ;

    evio_poll_start(loop, &loop->event);
    evio_unref(loop);
}

/**
 * @brief Drains the eventfd counter.
 * @details Retries on EINTR. On EAGAIN: nothing to drain. Other errors abort.
 * @param fd The eventfd file descriptor.
 */
static void evio_eventfd_drain(int fd)
{
    for (eventfd_t val = 1; /**/; val = 1) {
        ssize_t res = EVIO_EVENTFD_READ(fd, &val, sizeof(val));
        if (__evio_likely(res >= 0)) {
            break;
        }

        // GCOVR_EXCL_START
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (__evio_unlikely(err != EAGAIN)) {
            EVIO_ABORT("eventfd read failed, error %d: %s\n",
                       err, EVIO_STRERROR(err));
        }
        break;
        // GCOVR_EXCL_STOP
    }
}

/**
 * @brief Writes to the eventfd to signal it.
 * @details Retries on EINTR. On EAGAIN (counter max): drain once and retry.
 * @param fd The eventfd file descriptor.
 */
static void evio_eventfd_notify(int fd)
{
    for (eventfd_t val = 1; /**/; val = 1) {
        ssize_t res = EVIO_EVENTFD_WRITE(fd, &val, sizeof(val));
        if (__evio_likely(res >= 0)) {
            break;
        }

        int err = errno;
        // GCOVR_EXCL_START
        if (err == EINTR) {
            continue;
        }
        // GCOVR_EXCL_STOP

        if (__evio_unlikely(err != EAGAIN)) {
            EVIO_ABORT("eventfd write failed, error %d: %s\n",
                       err, EVIO_STRERROR(err));
        }

        evio_eventfd_drain(fd);
    }
}

void evio_eventfd_write(evio_loop *loop)
{
    if (atomic_exchange_explicit(&loop->event_pending.value, 1, memory_order_acq_rel)) {
        return;
    }

    if (!atomic_load_explicit(&loop->eventfd_allow.value, memory_order_acquire)) {
        return;
    }

    int err = errno;
    evio_eventfd_notify(loop->event.fd);
    errno = err;
}

void evio_eventfd_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    EVIO_ASSERT(base == &loop->event.base);

    atomic_store_explicit(&loop->event_pending.value, 0, memory_order_release);

    evio_signal_process_pending(loop);

    if (atomic_exchange_explicit(&loop->async_pending.value, 0, memory_order_acq_rel)) {
        for (size_t i = loop->async.count; i--;) {
            evio_async *w = container_of(loop->async.ptr[i], evio_async, base);

            if (atomic_exchange_explicit(&w->status.value, 0, memory_order_acq_rel)) {
                evio_queue_event(loop, &w->base, EVIO_ASYNC);
            }
        }
    }
}
