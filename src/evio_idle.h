#pragma once

/**
 * @file evio_idle.h
 * @brief An idle watcher is invoked when the event loop has no other events.
 *
 * Its callback is invoked when the event loop has finished processing all
 * pending events and is about to block waiting for new I/O. This makes it
 * suitable for work that should only run when the application is inactive.
 */

#include "evio.h"

/** @brief An idle watcher, invoked when the loop has no pending events. */
typedef struct evio_idle {
    EVIO_BASE;
} evio_idle;

/**
 * @brief Initializes an idle watcher.
 * @param w The idle watcher to initialize.
 * @param cb The callback to invoke when the loop is idle.
 */
static inline __evio_nonnull(1, 2)
void evio_idle_init(evio_idle *w, evio_cb cb)
{
    evio_init(&w->base, cb);
}

/**
 * @brief Starts an idle watcher, making it active in the event loop.
 * @param loop The event loop.
 * @param w The idle watcher to start.
 */
__evio_public __evio_nonnull(1, 2)
void evio_idle_start(evio_loop *loop, evio_idle *w);

/**
 * @brief Stops an idle watcher, deactivating it.
 * @param loop The event loop.
 * @param w The idle watcher to stop.
 */
__evio_public __evio_nonnull(1, 2)
void evio_idle_stop(evio_loop *loop, evio_idle *w);
