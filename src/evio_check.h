#pragma once

/**
 * @file evio_check.h
 * @brief A check watcher invoked after I/O and other events in a loop iteration.
 *
 * An `evio_check` watcher's callback is invoked after the event loop has
 * handled all I/O and other pending events for a given iteration. This provides
 * a clean point to execute logic that needs to react to state changes that may
 * have occurred during the event processing phase of the same loop cycle. They
 * run at the end of an iteration, just before the loop decides whether to
 * continue or terminate.
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
