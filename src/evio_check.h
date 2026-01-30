#pragma once

/**
 * @file evio_check.h
 * @brief A check watcher invoked after I/O and other events in a loop iteration.
 */

#include "evio.h"

/** @brief A check watcher, invoked after the main event processing phase. */
typedef struct evio_check {
    EVIO_BASE;
} evio_check;

/**
 * @brief Initializes a check watcher.
 * @param w The check watcher to initialize.
 * @param cb The callback to invoke.
 */
static inline __evio_nonnull(1, 2)
void evio_check_init(evio_check *w, evio_cb cb)
{
    evio_init(&w->base, cb);
}

/**
 * @brief Starts a check watcher, making it active in the event loop.
 * @param loop The event loop.
 * @param w The check watcher to start.
 */
__evio_public __evio_nonnull(1, 2)
void evio_check_start(evio_loop *loop, evio_check *w);

/**
 * @brief Stops a check watcher, deactivating it.
 * @param loop The event loop.
 * @param w The check watcher to stop.
 */
__evio_public __evio_nonnull(1, 2)
void evio_check_stop(evio_loop *loop, evio_check *w);
