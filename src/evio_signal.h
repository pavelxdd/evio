#pragma once

/**
 * @file evio_signal.h
 * @brief A signal watcher for handling POSIX signals as events.
 *
 * It allows an application to process signals as regular events within the main
 * loop's thread. This is achieved safely by capturing the signal in a minimal
 * handler and using an `eventfd` to delegate the actual processing to a
 * callback, avoiding the restrictions of running code in a signal handler
 * context.
 */

#include "evio.h"

/** @brief A signal watcher for handling POSIX signals. */
typedef struct evio_signal {
    EVIO_BASE;
    int signum; /**< The signal number to watch (e.g., `SIGINT`). */
} evio_signal;

/**
 * @brief Sets the signal number for a signal watcher.
 * @param w The signal watcher to modify.
 * @param signum The signal number (e.g., `SIGINT`).
 */
static inline __evio_nonnull(1)
void evio_signal_set(evio_signal *w, int signum)
{
    EVIO_ASSERT(signum > 0);
    EVIO_ASSERT(signum < NSIG);
    w->signum = signum;
}

/**
 * @brief Initializes a signal watcher.
 * @param w The signal watcher to initialize.
 * @param cb The callback to invoke when the signal is received.
 * @param signum The signal number to watch.
 */
static inline __evio_nonnull(1, 2)
void evio_signal_init(evio_signal *w, evio_cb cb, int signum)
{
    evio_init(&w->base, cb);
    evio_signal_set(w, signum);
}

/**
 * @brief Starts a signal watcher, making it active in the event loop.
 * @param loop The event loop.
 * @param w The signal watcher to start.
 */
__evio_public __evio_nonnull(1, 2)
void evio_signal_start(evio_loop *loop, evio_signal *w);

/**
 * @brief Stops a signal watcher, deactivating it.
 * @param loop The event loop.
 * @param w The signal watcher to stop.
 */
__evio_public __evio_nonnull(1, 2)
void evio_signal_stop(evio_loop *loop, evio_signal *w);
