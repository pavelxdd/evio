#pragma once

/**
 * @file evio_eventfd.h
 * @brief Internal module for managing an `eventfd` for loop wake-ups.
 */

#include "evio.h"

/**
 * @brief Ensures the eventfd watcher is initialized.
 * @details Called when async/signal watchers start.
 * @param loop The event loop.
 */
__evio_nonnull(1)
void evio_eventfd_init(evio_loop *loop);

/**
 * @brief Writes to the eventfd to wake up a sleeping event loop.
 * @param loop The event loop.
 */
__evio_nonnull(1)
void evio_eventfd_write(evio_loop *loop);

/**
 * @brief The internal callback for handling eventfd notifications.
 * @param loop The event loop.
 * @param base The event poll watcher.
 * @param emask The received event mask.
 */
__evio_nonnull(1, 2)
void evio_eventfd_cb(evio_loop *loop, evio_base *base, evio_mask emask);
