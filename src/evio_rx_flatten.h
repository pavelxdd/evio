#pragma once

/**
 * @file evio_rx_flatten.h
 * @brief Rx operators running an inner description per outer value.
 */

#include "evio_rx.h"

/**
 * @brief Returns an owned inner description for an outer value.
 * @details Returns NULL with errno set on failure; Rx releases the reference.
 */
typedef evio_rx_observable *(*evio_rx_project_cb)(void *ctx, const void *value);

/** @brief Output type, projector and its context of a flattening operator. */
typedef struct {
    const evio_rx_type *type;
    evio_rx_project_cb cb;
    evio_rx_capture capture;
} evio_rx_project;

/**
 * @brief Runs only the inner of the latest outer value.
 * @details Cancels the previous inner before calling the projector. Complete waits
 * for the current inner.
 * @param source The outer input.
 * @param project The projector.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_switch_map(evio_rx_observable *source, evio_rx_project project);

/**
 * @brief Runs inners one at a time in outer order.
 * @details Waiting outer values are projected on activation.
 * @param source The outer input.
 * @param project The projector.
 * @param capacity Positive FIFO capacity of waiting outer values.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_concat_map(evio_rx_observable *source, evio_rx_project project,
                                       size_t capacity);

/**
 * @brief Runs up to `concurrency` inners at once.
 * @details Free slots take waiting outer values in FIFO order.
 * @param source The outer input.
 * @param project The projector.
 * @param concurrency Positive limit of active inners.
 * @param capacity Positive FIFO capacity of waiting outer values.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_merge_map(evio_rx_observable *source, evio_rx_project project,
                                      size_t concurrency, size_t capacity);
