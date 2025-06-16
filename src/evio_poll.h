#pragma once

/**
 * @file evio_poll.h
 * @brief An I/O watcher for monitoring file descriptor readiness.
 *
 * This is the core mechanism for integrating non-blocking I/O operations with
 * the event loop. The implementation is backed by the system's `epoll`
 * interface.
 */

#include "evio.h"

/** @brief A poll watcher for monitoring file descriptor I/O events. */
typedef struct evio_poll {
    EVIO_BASE;
    int fd;             /**< The file descriptor to monitor. */
    evio_mask emask;    /**< The event mask (`EVIO_READ` or `EVIO_WRITE`). */
} evio_poll;

/**
 * @brief Modifies the event mask for a poll watcher.
 * This function does not restart the watcher if it's already running.
 * @param w The poll watcher to modify.
 * @param emask The new event mask (`EVIO_READ` or `EVIO_WRITE`).
 */
static inline __evio_nonnull(1)
void evio_poll_modify(evio_poll *w, evio_mask emask)
{
    w->emask = (emask & (EVIO_READ | EVIO_WRITE)) | (w->emask & EVIO_POLL);
}

/**
 * @brief Sets the file descriptor and event mask for a poll watcher.
 * This function does not start the watcher.
 * @param w The poll watcher to set up.
 * @param fd The file descriptor to monitor.
 * @param emask The event mask (`EVIO_READ` or `EVIO_WRITE`).
 */
static inline __evio_nonnull(1)
void evio_poll_set(evio_poll *w, int fd, evio_mask emask)
{
    EVIO_ASSERT(fd >= 0);
    w->fd = fd;
    w->emask = (emask & (EVIO_READ | EVIO_WRITE)) | EVIO_POLL;
}

/**
 * @brief Initializes a poll watcher.
 * @param w The poll watcher to initialize.
 * @param cb The callback to invoke for I/O events.
 * @param fd The file descriptor to monitor.
 * @param emask The event mask (`EVIO_READ` or `EVIO_WRITE`).
 */
static inline __evio_nonnull(1, 2)
void evio_poll_init(evio_poll *w, evio_cb cb, int fd, evio_mask emask)
{
    evio_init(&w->base, cb);
    evio_poll_set(w, fd, emask);
}

/**
 * @brief Starts a poll watcher, making it active in the event loop.
 * @param loop The event loop.
 * @param w The poll watcher to start.
 */
__evio_public __evio_nonnull(1, 2)
void evio_poll_start(evio_loop *loop, evio_poll *w);

/**
 * @brief Stops a poll watcher, deactivating it.
 * @param loop The event loop.
 * @param w The poll watcher to stop.
 */
__evio_public __evio_nonnull(1, 2)
void evio_poll_stop(evio_loop *loop, evio_poll *w);

/**
 * @brief Changes the file descriptor and/or events for a running poll watcher.
 * This function will restart the watcher if necessary.
 * @param loop The event loop.
 * @param w The poll watcher to change.
 * @param fd The new file descriptor.
 * @param emask The new event mask (`EVIO_READ` or `EVIO_WRITE`).
 */
__evio_public __evio_nonnull(1, 2)
void evio_poll_change(evio_loop *loop, evio_poll *w, int fd, evio_mask emask);
