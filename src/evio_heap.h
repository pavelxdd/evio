#pragma once

/**
 * @file evio_heap.h
 * @brief An internal binary min-heap implementation for timer management.
 *
 * The heap stores `evio_node` objects, which are (time, watcher) pairs, ordered
 * by expiration time. This allows the event loop to efficiently determine the
 * next timeout for `epoll_pwait`. A watcher's position in the heap is stored as
 * a 1-based index in its `active` field; a value of 0 indicates that the
 * watcher is not currently in the heap.
 */

#include "evio.h"

/** @brief A heap node representing a timer expiry point. */
typedef struct {
    evio_base *base;    /**< Pointer to the timer watcher's base. */
    evio_time time;     /**< The absolute expiration time in nanoseconds. */
} evio_node;

/**
 * @brief Sifts a heap element up to its correct position.
 * @param heap The heap array.
 * @param index The index of the element to sift up.
 */
__evio_nonnull(1)
void evio_heap_up(evio_node *heap, size_t index);

/**
 * @brief Sifts a heap element down to its correct position.
 * @param heap The heap array.
 * @param index The index of the element to sift down.
 * @param count The total number of elements in the heap.
 */
__evio_nonnull(1)
void evio_heap_down(evio_node *heap, size_t index, size_t count);

/**
 * @brief Adjusts a heap element's position, sifting it up or down as required.
 * @param heap The heap array.
 * @param index The index of the element to adjust.
 * @param count The total number of elements in the heap.
 */
__evio_nonnull(1)
void evio_heap_adjust(evio_node *heap, size_t index, size_t count);
