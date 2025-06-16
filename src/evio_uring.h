#pragma once

/**
 * @file evio_uring.h
 * @brief Internal module for optional `io_uring` support.
 *
 * It abstracts the `io_uring` API to submit `epoll_ctl` operations
 * asynchronously, which can offer a significant performance benefit over
 * traditional syscalls on supported systems.
 */

#include "evio.h"

struct epoll_event;

#ifdef EVIO_IO_URING

/** @brief Opaque type for an io_uring instance. */
typedef struct evio_uring evio_uring;

/**
 * @brief Creates and initializes a new io_uring instance.
 * @return A pointer to the new instance, or NULL if not supported or on error.
 */
__evio_nodiscard
evio_uring *evio_uring_new(void);

/**
 * @brief Frees an io_uring instance and its associated resources.
 * @param iou The io_uring instance to free.
 */
__evio_nonnull(1)
void evio_uring_free(evio_uring *iou);

/**
 * @brief Queues an epoll_ctl operation to be submitted via io_uring.
 * @param loop The event loop.
 * @param op The epoll operation (e.g., `EPOLL_CTL_ADD`, `EPOLL_CTL_MOD`).
 * @param fd The file descriptor for the operation.
 * @param ev The epoll_event structure for the operation.
 */
__evio_nonnull(1, 4)
void evio_uring_ctl(evio_loop *loop, int op, int fd, const struct epoll_event *ev);

/**
 * @brief Submits all pending io_uring operations and processes their results.
 * @param loop The event loop.
 */
__evio_nonnull(1)
void evio_uring_flush(evio_loop *loop);

#else // EVIO_IO_URING

// Stubs for when io_uring is not available
typedef void evio_uring;

static inline __evio_nodiscard
evio_uring *evio_uring_new(void)
{
    return NULL;
}

static inline __evio_nonnull(1)
void evio_uring_free(evio_uring *iou)
{
    (void)iou;
}

static inline __evio_nonnull(1, 4)
void evio_uring_ctl(evio_loop *loop, int op, int fd, const struct epoll_event *ev)
{
    (void)loop;
    (void)op;
    (void)fd;
    (void)ev;
}

static inline __evio_nonnull(1)
void evio_uring_flush(evio_loop *loop)
{
    (void)loop;
}

#endif // EVIO_IO_URING
