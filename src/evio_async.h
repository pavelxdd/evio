#pragma once

/**
 * @file evio_async.h
 * @brief An async watcher for thread-safe event loop wake-ups.
 *
 * An `evio_async` watcher provides a mechanism to wake up the event loop and
 * schedule a callback from any thread. The `evio_async_send()` function is the
 * only function in the `evio` API that is safe to call from a different thread
 * to interact with a running event loop. This mechanism is crucial for inter-thread
 * communication and is typically implemented using an `eventfd` for efficiency.
 */

#include "evio.h"

/** @brief An async watcher that can be safely triggered from another thread. */
typedef struct evio_async {
    EVIO_BASE;
    EVIO_ATOMIC(int) status; /**< @private The pending status of the watcher. */
} evio_async;

/**
 * @brief Checks if an async watcher has a pending event.
 * @param w The async watcher to check.
 * @return `true` if an event is pending, `false` otherwise.
 */
static inline __evio_nonnull(1) __evio_nodiscard
bool evio_async_pending(evio_async *w)
{
    return atomic_load_explicit(&w->status.value, memory_order_acquire);
}

/**
 * @brief Initializes an async watcher.
 * @param w The async watcher to initialize.
 * @param cb The callback to invoke when an event is received.
 */
static inline __evio_nonnull(1, 2)
void evio_async_init(evio_async *w, evio_cb cb)
{
    evio_init(&w->base, cb);
    atomic_init(&w->status.value, 0);
}

/**
 * @brief Starts an async watcher, making it active in the event loop.
 * @param loop The event loop.
 * @param w The async watcher to start.
 */
__evio_public __evio_nonnull(1, 2)
void evio_async_start(evio_loop *loop, evio_async *w);

/**
 * @brief Stops an async watcher, deactivating it.
 * @param loop The event loop.
 * @param w The async watcher to stop.
 */
__evio_public __evio_nonnull(1, 2)
void evio_async_stop(evio_loop *loop, evio_async *w);

/**
 * @brief Sends an event to an async watcher from any thread.
 * This function is thread-safe and will wake up the event loop if it is
 * sleeping.
 * @param loop The event loop to wake up.
 * @param w The async watcher to signal.
 */
__evio_public __evio_nonnull(1, 2)
void evio_async_send(evio_loop *loop, evio_async *w);
