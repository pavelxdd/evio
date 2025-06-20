#pragma once

/**
 * @file evio_cleanup.h
 * @brief A cleanup watcher that runs just before the event loop is freed.
 *
 * An `evio_cleanup` watcher provides a mechanism for resource deallocation tied
 * to the event loop's lifetime. Its callback is guaranteed to execute
 * immediately before the loop is freed via `evio_loop_free()`, making it the
 * ideal place to release memory or other resources associated with the loop.
 *
 * @note Cleanup watchers do not affect the loop's reference count and thus do
 * not keep the loop alive on their own.
 */

#include "evio.h"

/** @brief A cleanup watcher, invoked just before the event loop is freed. */
typedef struct evio_cleanup {
    EVIO_BASE;
} evio_cleanup;

/**
 * @brief Initializes a cleanup watcher.
 * @param w The cleanup watcher to initialize.
 * @param cb The callback to invoke for cleanup.
 */
static inline __evio_nonnull(1, 2)
void evio_cleanup_init(evio_cleanup *w, evio_cb cb)
{
    evio_init(&w->base, cb);
}

/**
 * @brief Starts a cleanup watcher, making it active in the event loop.
 * @param loop The event loop.
 * @param w The cleanup watcher to start.
 */
__evio_public __evio_nonnull(1, 2)
void evio_cleanup_start(evio_loop *loop, evio_cleanup *w);

/**
 * @brief Stops a cleanup watcher, deactivating it.
 * @param loop The event loop.
 * @param w The cleanup watcher to stop.
 */
__evio_public __evio_nonnull(1, 2)
void evio_cleanup_stop(evio_loop *loop, evio_cleanup *w);
