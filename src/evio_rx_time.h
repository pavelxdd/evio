#pragma once

/**
 * @file evio_rx_time.h
 * @brief Time-based Rx operators.
 *
 * Durations are nanoseconds of the cached monotonic loop time; call
 * `evio_update_time()` when a fresh base is needed outside loop iterations or
 * after long work. Deadline overflow fails the run with EOVERFLOW.
 */

#include "evio_rx.h"

/**
 * @brief Emits the last value after `duration` of silence.
 * @details Complete flushes the pending value; error and cancel discard it.
 * @param source The input.
 * @param duration The positive window.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_debounce_time(evio_rx_observable *source, evio_time duration);

/**
 * @brief Emits a value, then drops values until `duration` has passed.
 * @details Leading only; a value at the deadline starts a new window.
 * @param source The input.
 * @param duration The positive window.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_throttle_time(evio_rx_observable *source, evio_time duration);

/**
 * @brief Delays every value by `duration` through a FIFO.
 * @details Duration 0 is still asynchronous. Complete waits for the FIFO; error
 * and cancel discard it. Overflow fails the result with EOVERFLOW.
 * @param source The input.
 * @param duration The delay.
 * @param capacity Positive FIFO capacity.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_delay(evio_rx_observable *source, evio_time duration, size_t capacity);

/**
 * @brief Fails with ETIMEDOUT when no value arrives within `duration`.
 * @details Applies before the first value, between values and before complete. A
 * value or complete at the deadline is late.
 * @param source The input.
 * @param duration The positive limit.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_timeout(evio_rx_observable *source, evio_time duration);
