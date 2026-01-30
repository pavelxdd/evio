#pragma once

/**
 * @file evio_timer.h
 * @brief A timer watcher for scheduling time-based events.
 */

#include "evio.h"

/** @brief A timer watcher for scheduling time-based events. */
typedef struct evio_timer {
    EVIO_BASE;
    evio_time repeat; /**< The repeat interval in nanoseconds. If 0, the timer is one-shot. */
} evio_timer;

/**
 * @brief Sets the repeat interval for a timer watcher.
 * @param w The timer watcher to modify.
 * @param repeat The repeat interval in nanoseconds.
 */
static inline __evio_nonnull(1)
void evio_timer_set(evio_timer *w, evio_time repeat)
{
    w->repeat = repeat;
}

/**
 * @brief Initializes a timer watcher.
 * @param w The timer watcher to initialize.
 * @param cb The callback to invoke when the timer expires.
 * @param repeat The repeat interval in nanoseconds. Set to 0 for a one-shot timer.
 */
static inline __evio_nonnull(1, 2)
void evio_timer_init(evio_timer *w, evio_cb cb, evio_time repeat)
{
    evio_init(&w->base, cb);
    evio_timer_set(w, repeat);
}

/**
 * @brief Starts a timer watcher with a given timeout.
 * @param loop The event loop.
 * @param w The timer watcher to start.
 * @param after The initial timeout in nanoseconds.
 */
__evio_public __evio_nonnull(1, 2)
void evio_timer_start(evio_loop *loop, evio_timer *w, evio_time after);

/**
 * @brief Stops a timer watcher, deactivating it.
 * @param loop The event loop.
 * @param w The timer watcher to stop.
 */
__evio_public __evio_nonnull(1, 2)
void evio_timer_stop(evio_loop *loop, evio_timer *w);

/**
 * @brief Stops a one-shot timer or restarts a repeating timer.
 * @details Repeating timers are rescheduled relative to current loop time.
 * @param loop The event loop.
 * @param w The timer watcher to restart.
 */
__evio_public __evio_nonnull(1, 2)
void evio_timer_again(evio_loop *loop, evio_timer *w);

/**
 * @brief Gets the remaining time until the timer is next scheduled to fire.
 * @param loop The event loop.
 * @param w The timer watcher to check.
 * @return The remaining time in nanoseconds, or 0 if the timer is not active
 *         or has already expired.
 */
__evio_public __evio_nonnull(1, 2) __evio_nodiscard
evio_time evio_timer_remaining(const evio_loop *loop, const evio_timer *w);
