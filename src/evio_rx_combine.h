#pragma once

/**
 * @file evio_rx_combine.h
 * @brief Rx operators over several inputs.
 *
 * Input arrays are nonempty and copied; inputs start in array order.
 */

#include "evio_rx.h"

/**
 * @brief Builds one output value from one value per input.
 * @details `values` are borrowed, in input order, for the duration of the call.
 */
typedef int (*evio_rx_combine_cb)(void *ctx, void *out, const void *const *values, size_t count);

/** @brief Output type, callback and its context of a combining operator. */
typedef struct {
    const evio_rx_type *type;
    evio_rx_combine_cb cb;
    evio_rx_capture capture;
} evio_rx_combiner;

/**
 * @brief Forwards values of all inputs; completes after all inputs.
 * @param sources Inputs with an identical type descriptor.
 * @param count The number of inputs.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_merge(evio_rx_observable *const *sources, size_t count);

/**
 * @brief Forwards `source` until `notifier` emits a value.
 * @details The notifier starts first. Its next completes the result; its complete
 * without a value is ignored.
 * @param source The input.
 * @param notifier The stop signal of any type.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_take_until(evio_rx_observable *source, evio_rx_observable *notifier);

/**
 * @brief Combines the latest value of every input on each change.
 * @param sources Inputs of any types.
 * @param count The number of inputs.
 * @param combiner The output type and callback.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_combine_latest(evio_rx_observable *const *sources, size_t count,
                                           evio_rx_combiner combiner);

/**
 * @brief Combines the n-th values of all inputs.
 * @details Each input has its own FIFO; overflow fails the result with EOVERFLOW.
 * Completes once a completed input has no value for the next combination.
 * @param sources Inputs of any types.
 * @param count The number of inputs.
 * @param combiner The output type and callback.
 * @param capacity Positive capacity of each input FIFO.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_zip(evio_rx_observable *const *sources, size_t count,
                                evio_rx_combiner combiner, size_t capacity);
