#pragma once

/**
 * @file evio_loop.h
 * @brief Public API for core event loop management.
 *
 * Loop lifecycle (`evio_loop_new` / `evio_loop_free`), execution (`evio_run` /
 * `evio_break`), and helpers for manual event injection.
 */

#include "evio.h"

/**
 * @brief Creates a new event loop.
 * @param flags Flags to customize loop creation (e.g., `EVIO_FLAG_URING`).
 * @return A new event loop instance, or NULL on failure.
 */
__evio_public __evio_nodiscard
evio_loop *evio_loop_new(int flags);

/**
 * @brief Frees an event loop and all associated resources.
 * Invokes cleanup watchers.
 * @param loop The event loop to free.
 */
__evio_public __evio_nonnull(1)
void evio_loop_free(evio_loop *loop);

/**
 * @brief Returns the loop's cached monotonic time in nanoseconds.
 * @param loop The event loop.
 * @return The cached time in nanoseconds.
 */
__evio_public __evio_nonnull(1) __evio_nodiscard
evio_time evio_get_time(const evio_loop *loop);

/**
 * @brief Updates the loop's cached monotonic time to the current value.
 * @details Timers use `loop->time`. The loop updates it once per iteration; call
 * this if you need a refresh inside a long callback or from a prepare watcher.
 * @param loop The event loop to update.
 */
__evio_public __evio_nonnull(1)
void evio_update_time(evio_loop *loop);

/**
 * @brief Increments the loop's reference count.
 * The loop will continue to run as long as the reference count is > 0.
 * @param loop The event loop to reference.
 */
__evio_public __evio_nonnull(1)
void evio_ref(evio_loop *loop);

/**
 * @brief Decrements the loop's reference count.
 * @param loop The event loop to unreference.
 */
__evio_public __evio_nonnull(1)
void evio_unref(evio_loop *loop);

/**
 * @brief Gets the loop's current reference count.
 * @param loop The event loop.
 * @return The current reference count.
 */
__evio_public __evio_nonnull(1) __evio_nodiscard
size_t evio_refcount(const evio_loop *loop);

/**
 * @brief Associates a user-defined data pointer with the event loop.
 * @param loop The event loop.
 * @param data The user data pointer.
 */
__evio_public __evio_nonnull(1)
void evio_set_userdata(evio_loop *loop, void *data);

/**
 * @brief Retrieves the user-defined data pointer from the event loop.
 * @param loop The event loop.
 * @return The user data pointer.
 */
__evio_public __evio_nonnull(1) __evio_nodiscard
void *evio_get_userdata(const evio_loop *loop);

/**
 * @brief Sets the loop's clock source ID.
 * @param loop The event loop.
 * @param clock_id The new `clockid_t` to use (e.g., `CLOCK_MONOTONIC`).
 */
__evio_public __evio_nonnull(1)
void evio_set_clockid(evio_loop *loop, clockid_t clock_id);

/**
 * @brief Gets the loop's current clock source ID.
 * @param loop The event loop.
 * @return The `clockid_t` for the clock source being used.
 */
__evio_public __evio_nonnull(1) __evio_nodiscard
clockid_t evio_get_clockid(const evio_loop *loop);

/**
 * @brief Runs the event loop.
 * @details Returns 0 if `refcount == 0` or stopped via `EVIO_BREAK_ALL`.
 *
 * @param loop The event loop to run.
 * @param flags Flags to control execution (e.g., `EVIO_RUN_ONCE`).
 * @return 0 if there are no active watchers, non-zero otherwise.
 */
__evio_public __evio_nonnull(1)
int evio_run(evio_loop *loop, int flags);

/**
 * @brief Requests the event loop to stop running.
 * @details `EVIO_BREAK_ONE` returns from the current `evio_run`. `EVIO_BREAK_ALL`
 * returns from the current and all nested `evio_run` calls.
 * @param loop The event loop to stop.
 * @param state The break state (`EVIO_BREAK_ONE` or `EVIO_BREAK_ALL`).
 */
__evio_public __evio_nonnull(1)
void evio_break(evio_loop *loop, int state);

/**
 * @brief Gets the loop's current break state.
 * @param loop The event loop.
 * @return The current break state.
 */
__evio_public __evio_nonnull(1) __evio_nodiscard
int evio_break_state(const evio_loop *loop);

/**
 * @brief Queues an event for a watcher to be processed in the current iteration.
 * @param loop The event loop.
 * @param base The watcher to feed the event to.
 * @param emask The event mask to deliver.
 */
__evio_public __evio_nonnull(1, 2)
void evio_feed_event(evio_loop *loop, evio_base *base, evio_mask emask);

/**
 * @brief Queues an I/O event for all poll watchers on a file descriptor.
 * @param loop The event loop.
 * @param fd The file descriptor that has a pending event.
 * @param emask The I/O event mask (`EVIO_READ` or `EVIO_WRITE`).
 */
__evio_public __evio_nonnull(1)
void evio_feed_fd_event(evio_loop *loop, int fd, evio_mask emask);

/**
 * @brief Queues an I/O error for all poll watchers on a file descriptor.
 * @param loop The event loop.
 * @param fd The file descriptor that has an error.
 */
__evio_public __evio_nonnull(1)
void evio_feed_fd_error(evio_loop *loop, int fd);

/**
 * @brief Queues signal events for all watchers on a given signal number.
 * Does not raise a POSIX signal.
 * @param loop The event loop.
 * @param signum The signal number to feed.
 */
__evio_public __evio_nonnull(1)
void evio_feed_signal(evio_loop *loop, int signum);

/**
 * @brief Invokes all pending callbacks immediately.
 * @warning Re-entrant: calling from callbacks can recurse and exhaust the stack.
 * @param loop The event loop.
 */
__evio_public __evio_nonnull(1)
void evio_invoke_pending(evio_loop *loop);

/**
 * @brief Clears any pending event for a given watcher.
 * @param loop The event loop.
 * @param base The watcher whose pending event should be cleared.
 */
__evio_public __evio_nonnull(1, 2)
void evio_clear_pending(evio_loop *loop, evio_base *base);

/**
 * @brief Gets the current number of pending events in the loop.
 * @param loop The event loop.
 * @return The number of pending events.
 */
__evio_public __evio_nonnull(1) __evio_nodiscard
size_t evio_pending_count(const evio_loop *loop);
