#pragma once

/**
 * @file evio_eventfd.h
 * @brief Internal module for managing an `eventfd` for loop wake-ups.
 *
 * This is the low-level mechanism that underpins the functionality of async
 * and signal watchers, allowing them to safely notify the event loop from
 * external threads or signal handlers.
 */

#include "evio.h"

/**
 * @brief Ensures the eventfd watcher is initialized.
 * This is called automatically when an async or signal watcher is started.
 * @param loop The event loop.
 */
__evio_nonnull(1)
void evio_eventfd_init(evio_loop *loop);

/**
 * @brief Writes to the eventfd to wake up a sleeping event loop.
 * This function is for internal use by other evio components.
 * @param loop The event loop.
 */
__evio_nonnull(1)
void evio_eventfd_write(evio_loop *loop);

/**
 * @brief The internal callback for handling eventfd notifications.
 * It processes pending async and signal events.
 * @param loop The event loop.
 * @param base The event poll watcher.
 * @param emask The received event mask.
 */
__evio_nonnull(1, 2)
void evio_eventfd_cb(evio_loop *loop, evio_base *base, evio_mask emask);
