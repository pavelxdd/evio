#pragma once

/**
 * @file evio_rx.h
 * @brief Rx API umbrella header.
 *
 * Not included by `evio.h`. All Rx objects are loop-thread-only. Loops outlive
 * their subscriptions, Subjects and descriptions retaining Subjects.
 * Constructors return owned handles, or NULL with errno set on recoverable errors.
 * A runtime failure closes the run and calls `observer.error`, if supplied,
 * with a positive errno-compatible code. Internal allocator failures are fatal
 * (`EVIO_ABORT`) and are not reported through either channel.
 */

#include "evio.h"

/** @brief A reusable, reference-counted stream description. */
typedef struct evio_rx_observable evio_rx_observable;

/** @brief One run of a description on a loop. */
typedef struct evio_rx_subscription evio_rx_subscription;

/**
 * @brief A value type descriptor.
 * @details The descriptor and `ctx` are borrowed: they outlive every description,
 * run and retained value using them. Types match by descriptor identity.
 * `size` is positive; `alignment` is a power of two and divides `size`.
 * Without `copy`, values are copied bytewise and `destroy` must be NULL.
 * `copy` returns 0 or a positive errno, leaving no live `dst` on failure.
 */
typedef struct {
    size_t size;
    size_t alignment;
    int (*copy)(void *ctx, void *dst, const void *src);
    void (*destroy)(void *ctx, void *value);
    void *ctx;
} evio_rx_type;

/**
 * @brief Notification callbacks of a subscription.
 * @details Any callback may be NULL. Values are borrowed only during `next`.
 */
typedef struct {
    void (*next)(void *ctx, const void *value);
    void (*error)(void *ctx, int error);
    void (*complete)(void *ctx);
    void *ctx;
} evio_rx_observer;

/**
 * @brief Shared configuration of a description.
 * @details NULL `destroy` borrows `data`. Otherwise a successful constructor takes
 * ownership until the last description reference is released; failure leaves
 * ownership with the caller. Per-run state does not belong here.
 */
typedef struct {
    void *data;
    void (*destroy)(void *data);
} evio_rx_capture;

/**
 * @brief Returns an additional owned reference to a description.
 * @param source The description.
 * @return `source`.
 */
__evio_public __evio_nonnull(1) __evio_nodiscard
evio_rx_observable *evio_rx_observable_ref(evio_rx_observable *source);

/**
 * @brief Releases one description reference without cancelling runs.
 * @param source The description.
 */
__evio_public __evio_nonnull(1)
void evio_rx_observable_free(evio_rx_observable *source);

/**
 * @brief Prepares a run without notifying the observer.
 * @details Reentrant inputs are copied into a bounded FIFO; overflow terminates the
 * run with EOVERFLOW.
 * @param loop The loop the run belongs to.
 * @param source The description; the run takes its own reference.
 * @param observer The observer; `observer.ctx` is borrowed until free.
 * @param queue_capacity Positive capacity of the reentry FIFO.
 * @return The subscription, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_subscription *evio_rx_subscription_new(evio_loop *loop, evio_rx_observable *source,
                                               evio_rx_observer observer, size_t queue_capacity);

/**
 * @brief Starts a prepared run; may notify synchronously. Later calls do nothing.
 * @param subscription The subscription.
 */
__evio_public __evio_nonnull(1)
void evio_rx_subscription_start(evio_rx_subscription *subscription);

/**
 * @brief Cancels the current run without a terminal notification and starts a new one.
 * @details The new run reuses the memory of the closed one and notifies the same
 * observer. Called from the run's own callbacks, it starts once they return.
 * @param subscription The subscription.
 */
__evio_public __evio_nonnull(1)
void evio_rx_subscription_restart(evio_rx_subscription *subscription);

/**
 * @brief Cancels the run without a terminal notification.
 * @details A closed handle remains valid until free or restart. Values held by
 * operators are released once the run is closed; the description is kept until free.
 * @param subscription The subscription.
 */
__evio_public __evio_nonnull(1)
void evio_rx_subscription_cancel(evio_rx_subscription *subscription);

/**
 * @brief Checks whether the run is closed by a terminal event or cancel.
 * @param subscription The subscription.
 * @return `true` if closed.
 */
__evio_public __evio_nonnull(1) __evio_nodiscard
bool evio_rx_subscription_is_closed(const evio_rx_subscription *subscription);

/**
 * @brief Cancels and releases the subscription.
 * @details Allowed from the run's own callbacks; the pointer is invalid afterwards.
 * @param subscription The subscription.
 */
__evio_public __evio_nonnull(1)
void evio_rx_subscription_free(evio_rx_subscription *subscription);

// IWYU pragma: begin_exports

#include "evio_rx_source.h"
#include "evio_rx_subject.h"
#include "evio_rx_ops.h"
#include "evio_rx_combine.h"
#include "evio_rx_time.h"
#include "evio_rx_flatten.h"

// IWYU pragma: end_exports
