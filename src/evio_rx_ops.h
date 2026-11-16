#pragma once

/**
 * @file evio_rx_ops.h
 * @brief Synchronous single-input Rx operators.
 *
 * Operators retain their input description; `capture.data` is the callback `ctx`.
 * Transform callbacks construct `out` and return 0, or return a positive errno
 * leaving no live `out`.
 */

#include "evio_rx.h"

/** @brief Builds one output value from an input value. */
typedef int (*evio_rx_map_cb)(void *ctx, void *out, const void *value);

/** @brief Selects input values to pass. */
typedef bool (*evio_rx_filter_cb)(void *ctx, const void *value);

/** @brief Builds the next accumulator from the previous one and a value. */
typedef int (*evio_rx_scan_cb)(void *ctx, void *out, const void *accumulator, const void *value);

/** @brief Compares a value with the previously emitted one. */
typedef bool (*evio_rx_equal_cb)(void *ctx, const void *previous, const void *value);

/** @brief Builds `count` output values in `out` from `count` contiguous input values. */
typedef int (*evio_rx_map_batch_cb)(void *ctx, void *out, const void *values, size_t count);

/**
 * @brief Copies the selected values of `count` contiguous input values to `out`.
 * @details Copies keep input order. Stores their number in `*kept` on success.
 */
typedef int (*evio_rx_filter_batch_cb)(void *ctx, void *out, size_t *kept, const void *values,
                                       size_t count);

/**
 * @brief Transforms every value; a callback failure fails the run.
 * @param source The input.
 * @param type The output value type.
 * @param cb The transform.
 * @param capture The callback context.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_map(evio_rx_observable *source, const evio_rx_type *type,
                                evio_rx_map_cb cb, evio_rx_capture capture);

/**
 * @brief Passes values accepted by `cb`, without retaining them.
 * @param source The input.
 * @param cb The predicate.
 * @param capture The callback context.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_filter(evio_rx_observable *source,
                                   evio_rx_filter_cb cb, evio_rx_capture capture);

/**
 * @brief Emits every new accumulator; `seed` itself is not emitted.
 * @param source The input.
 * @param type The accumulator type.
 * @param seed The initial accumulator, copied into the description and each run.
 * @param cb The accumulator step.
 * @param capture The callback context.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_scan(evio_rx_observable *source, const evio_rx_type *type,
                                 const void *seed, evio_rx_scan_cb cb, evio_rx_capture capture);

/**
 * @brief Drops values equal to an owned copy of the last emitted value.
 * @param source The input.
 * @param cb The required comparison.
 * @param capture The callback context.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_distinct_until_changed(evio_rx_observable *source,
                                                   evio_rx_equal_cb cb, evio_rx_capture capture);

/**
 * @brief Transforms values in batches; a callback failure fails the run.
 * @details Batches come from `evio_rx_from_array()`, `evio_rx_subject_next_batch()`
 * and batch operators and pass through `evio_rx_merge()` and `evio_rx_take_until()`;
 * other inputs deliver one value at a time. A cancel during delivery discards the
 * rest of the built values.
 * @param source The input.
 * @param type The output value type.
 * @param cb The transform.
 * @param capture The callback context.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_map_batch(evio_rx_observable *source, const evio_rx_type *type,
                                      evio_rx_map_batch_cb cb, evio_rx_capture capture);

/**
 * @brief Passes values selected by `cb` in batches as copies.
 * @details Batches as in `evio_rx_map_batch()`.
 * @param source The input.
 * @param cb The selection.
 * @param capture The callback context.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_filter_batch(evio_rx_observable *source,
                                         evio_rx_filter_batch_cb cb, evio_rx_capture capture);
