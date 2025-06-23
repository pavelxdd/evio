#pragma once

/**
 * @file evio_list.h
 * @brief Internal utilities for a lightweight, resizable dynamic array (list).
 *
 * This module provides a simple and efficient way to manage collections of
 * watchers (e.g., idle, check, prepare) throughout the library, avoiding
 * heavier container abstractions. The utilities handle dynamic resizing and the
 * addition/removal of watchers from these lists.
 */

#include "evio.h"

/**
 * @brief Defines a generic dynamic array (list) structure.
 * @param type The type of elements stored in the list.
 */
#define EVIO_LIST(type) \
    struct { \
        type *ptr;      /**< Pointer to the allocated array of elements. */ \
        size_t count;   /**< The number of elements currently in the list. */ \
        size_t total;   /**< The total allocated capacity of the list. */ \
    }

/** @brief A generic list of watcher base pointers. */
typedef EVIO_LIST(evio_base *) evio_list;

/**
 * @brief Resizes a list if its capacity is less than the required count.
 * @param ptr The current pointer to the list's memory.
 * @param size The size of each element in the list.
 * @param count The required number of elements.
 * @param[in,out] total A pointer to the list's current capacity.
 * @return A pointer to the (potentially reallocated) list memory.
 */
__evio_nonnull(4) __evio_nodiscard __evio_returns_nonnull
void *evio_list_resize(void *ptr, size_t size, size_t count, size_t *total);

/**
 * @brief Starts a list-based watcher by adding it to a list.
 * @param loop The event loop.
 * @param w The watcher to start.
 * @param list The list to add the watcher to.
 * @param do_ref `true` to increment the loop's reference count.
 */
void evio_list_start(evio_loop *loop, evio_base *base,
                     evio_list *list, bool do_ref);

/**
 * @brief Stops a list-based watcher by removing it from a list.
 * @param loop The event loop.
 * @param w The watcher to stop.
 * @param list The list to remove the watcher from.
 * @param do_ref `true` to decrement the loop's reference count.
 */
void evio_list_stop(evio_loop *loop, evio_base *base,
                    evio_list *list, bool do_ref);
