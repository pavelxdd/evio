#pragma once

/**
 * @file evio_prepare.h
 * @brief A prepare watcher that runs at the beginning of each loop iteration.
 *
 * An `evio_prepare` watcher's callback is invoked at the start of each loop
 * iteration, just before the loop blocks to wait for I/O events. This allows
 * for setup tasks to be performed at a predictable point before any other event
 * handling in that iteration.
 */

#include "evio.h"

/** @brief A prepare watcher, invoked before the I/O polling phase. */
typedef struct evio_prepare {
    EVIO_BASE;
} evio_prepare;

/**
 * @brief Initializes a prepare watcher.
 * @param w The prepare watcher to initialize.
 * @param cb The callback to invoke.
 */
static inline __evio_nonnull(1, 2)
void evio_prepare_init(evio_prepare *w, evio_cb cb)
{
    evio_init(&w->base, cb);
}

/**
 * @brief Starts a prepare watcher, making it active in the event loop.
 * @param loop The event loop.
 * @param w The prepare watcher to start.
 */
__evio_public __evio_nonnull(1, 2)
void evio_prepare_start(evio_loop *loop, evio_prepare *w);

/**
 * @brief Stops a prepare watcher, deactivating it.
 * @param loop The event loop.
 * @param w The prepare watcher to stop.
 */
__evio_public __evio_nonnull(1, 2)
void evio_prepare_stop(evio_loop *loop, evio_prepare *w);
