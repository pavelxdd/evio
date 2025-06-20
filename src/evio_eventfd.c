#include <errno.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "evio_core.h"
#include "evio_eventfd.h"

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
 * @brief Writes to the eventfd to signal it.
 * @details This function attempts to write to the eventfd. If the write fails
 * with EAGAIN (because the eventfd counter is at its max), it performs a read
 * to reset the counter before attempting the write again. This ensures the
 * notification is delivered.
 * @param fd The eventfd file descriptor.
 */
static void evio_eventfd_notify(int fd)
{
    for (eventfd_t val = 1; /**/; val = 1) {
        ssize_t res = write(fd, &val, sizeof(val));
        if (__evio_likely(res >= 0)) {
            break;
        }

        // GCOVR_EXCL_START
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err != EAGAIN) {
            break;
        }
        // GCOVR_EXCL_STOP

        for (;;) {
            res = read(fd, &val, sizeof(val));
            // GCOVR_EXCL_START
            if (__evio_likely(res >= 0)) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            // GCOVR_EXCL_STOP
            break;
        }
    }
}

void evio_eventfd_write(evio_loop *loop)
{
    if (atomic_exchange_explicit(&loop->event_pending.value, 1, memory_order_acq_rel)) {
        return;
    }

    if (!atomic_load_explicit(&loop->eventfd_allow.value, memory_order_seq_cst)) {
        return;
    }

    atomic_store_explicit(&loop->event_pending.value, 0, memory_order_release);

    int err = errno;
    evio_eventfd_notify(loop->event.fd);
    errno = err;
}

void evio_eventfd_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    (void)base;
    (void)emask;

    EVIO_ASSERT(base == &loop->event.base);

    atomic_store_explicit(&loop->event_pending.value, 0, memory_order_release);

    evio_signal_process_pending(loop);

    if (atomic_exchange_explicit(&loop->async_pending.value, 0, memory_order_acq_rel)) {
        for (size_t i = loop->async.count; i--;) {
            evio_async *w = (evio_async *)(loop->async.ptr[i]);

            if (atomic_exchange_explicit(&w->status.value, 0, memory_order_acq_rel)) {
                evio_queue_event(loop, &w->base, EVIO_ASYNC);
            }
        }
    }
}
