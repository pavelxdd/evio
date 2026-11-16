#pragma once

/**
 * @file evio_rx_subject.h
 * @brief Hot Rx source published by the application.
 */

#include "evio_rx.h"

/** @brief A hot, loop-bound publisher without value replay. */
typedef struct evio_rx_subject evio_rx_subject;

/**
 * @brief Creates a Subject.
 * @param loop The loop; the Subject and descriptions retaining it are bound to it.
 * @param type The value type.
 * @param capacity Positive capacity of the FIFO of nested publications.
 * @return The Subject, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_subject *evio_rx_subject_new(evio_loop *loop, const evio_rx_type *type, size_t capacity);

/**
 * @brief Creates a description of a Subject's stream.
 * @details Late subscribers receive no earlier values, but do receive a cached
 * terminal event.
 * @param subject The Subject; the description retains it.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_from_subject(evio_rx_subject *subject);

/**
 * @brief Publishes a value to current subscribers synchronously.
 * @details Nested publications are queued. Failures local to one subscription
 * still return 0. Subject FIFO overflow fails the Subject and all subscribers
 * with EOVERFLOW.
 * @param subject The Subject.
 * @param value The borrowed value; NULL fails the Subject with EINVAL.
 * @return 0, or -1 with errno set; EPIPE once the Subject is closed.
 */
__evio_public __evio_nonnull(1)
int evio_rx_subject_next(evio_rx_subject *subject, const void *value);

/**
 * @brief Publishes `count` contiguous values like `evio_rx_subject_next()`.
 * @details Outside a delivery each subscriber receives the whole batch before
 * the next one; a nested batch is queued as single values.
 * @param subject The Subject.
 * @param values The borrowed values; NULL fails the Subject with EINVAL.
 * @param count The number of values; 0 publishes nothing.
 * @return 0, or -1 with errno set; EPIPE once the Subject is closed.
 */
__evio_public __evio_nonnull(1)
int evio_rx_subject_next_batch(evio_rx_subject *subject, const void *values, size_t count);

/**
 * @brief Fails the Subject, discarding queued values. Idempotent.
 * @param subject The Subject.
 * @param error The errno-compatible code; non-positive becomes EINVAL.
 */
__evio_public __evio_nonnull(1)
void evio_rx_subject_error(evio_rx_subject *subject, int error);

/**
 * @brief Completes the Subject after queued values. Idempotent.
 * @param subject The Subject.
 */
__evio_public __evio_nonnull(1)
void evio_rx_subject_complete(evio_rx_subject *subject);

/**
 * @brief Reopens a completed or failed Subject for new values.
 * @details Subscriptions that received the terminal event stay closed; restart
 * them with `evio_rx_subscription_restart()`. Does nothing for an open Subject.
 * @param subject The Subject.
 * @return 0, or -1 with errno EBUSY during a delivery of the Subject.
 */
__evio_public __evio_nonnull(1)
int evio_rx_subject_reopen(evio_rx_subject *subject);

/**
 * @brief Releases the publisher reference without completing the Subject.
 * @param subject The Subject.
 */
__evio_public __evio_nonnull(1)
void evio_rx_subject_free(evio_rx_subject *subject);
