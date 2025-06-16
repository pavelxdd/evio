#pragma once

/**
 * @file evio_once.h
 * @brief A one-shot watcher that triggers on I/O or a timeout.
 *
 * This watcher simplifies a common use case by internally combining the
 * functionality of a poll watcher and a timer watcher. It triggers on whichever
 * event occurs first.
 */

#include "evio.h"

/** @brief A one-shot watcher that triggers on I/O or a timeout. */
typedef struct evio_once {
    EVIO_BASE;
    evio_poll io;       /**< The internal poll watcher. */
    evio_timer tm;      /**< The internal timer watcher. */
} evio_once;

/**
 * @brief Initializes a once watcher.
 * @param w The once watcher to initialize.
 * @param cb The callback to invoke when the watcher triggers.
 * @param fd The file descriptor to monitor for I/O events.
 * @param emask The I/O event mask (`EVIO_READ` or `EVIO_WRITE`).
 */
__evio_public __evio_nonnull(1, 2)
void evio_once_init(evio_once *w, evio_cb cb, int fd, evio_mask emask);

/**
 * @brief Starts a once watcher with a specified timeout.
 * @param loop The event loop.
 * @param w The once watcher to start.
 * @param after The timeout in nanoseconds.
 */
__evio_public __evio_nonnull(1, 2)
void evio_once_start(evio_loop *loop, evio_once *w, evio_time after);

/**
 * @brief Stops a once watcher.
 * Deactivates the watcher and its underlying poll/timer watchers.
 * @param loop The event loop.
 * @param w The once watcher to stop.
 */
__evio_public __evio_nonnull(1, 2)
void evio_once_stop(evio_loop *loop, evio_once *w);
