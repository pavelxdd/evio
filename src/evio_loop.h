#pragma once

/**
 * @file evio_loop.h
 * @brief Public API for core event loop management.
 *
 * This module provides the functions necessary to create, run, and destroy an
 * event loop, as well as control its execution, manage its reference count,
 * and associate user data. It also includes functions for manually injecting
 * events into the loop.
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
 * This will invoke any active cleanup watchers before freeing memory.
 * @param loop The event loop to free.
 */
__evio_public __evio_nonnull(1)
void evio_loop_free(evio_loop *loop);

/**
 * @brief Returns the loop's cached monotonic time in nanoseconds.
 * This time is updated at the start of each loop iteration.
 * @param loop The event loop.
 * @return The cached time in nanoseconds.
 */
__evio_public __evio_nonnull(1) __evio_nodiscard
evio_time evio_get_time(const evio_loop *loop);

/**
 * @brief Updates the loop's cached monotonic time to the current value.
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
 * @brief Starts the event loop.
 * The loop runs until it is stopped via `evio_break` or has no active watchers
 * with reference counts.
 * @param loop The event loop to run.
 * @param flags Flags to control execution (e.g., `EVIO_RUN_ONCE`).
 * @return Non-zero if there are still active watchers, zero otherwise.
 */
__evio_public __evio_nonnull(1)
int evio_run(evio_loop *loop, int flags);

/**
 * @brief Requests the event loop to stop running.
 * @details This function is typically called from within a watcher callback.
 * `EVIO_BREAK_ONE` will cause the current `evio_run` to return, while
 * `EVIO_BREAK_ALL` will cause the current and all nested `evio_run` calls
 * to return.
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
 * This does not raise a POSIX signal itself but simulates its delivery.
 * @param loop The event loop.
 * @param signum The signal number to feed.
 */
__evio_public __evio_nonnull(1)
void evio_feed_signal(evio_loop *loop, int signum);

/**
 * @brief Invokes all pending callbacks immediately.
 * @details This function processes all events currently in the pending queue.
 *
 * @warning This function is re-entrant. If called from within a watcher
 * callback, it will immediately begin processing any newly queued events before
 * the current callback or the original `evio_invoke_pending` call returns. This
 * results in a depth-first event processing order. While this can be a powerful
 * feature for immediate, nested event handling, developers should be mindful
 * that deep recursion can lead to stack exhaustion.
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
