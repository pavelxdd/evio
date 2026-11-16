#pragma once

/**
 * @file evio_rx_buffer.h
 * @brief Internal storage of Rx values.
 *
 * Values are constructed and destroyed through their `evio_rx_type`. Storage
 * owned by a run comes from the run memory; descriptions and Subjects pass a
 * NULL run and use the heap. Storage is released with the same owner.
 */

#include <errno.h>
#include <string.h>

#include "evio_rx_node.h"

/** @brief Storage of up to a fixed number of values, filled from the front. */
typedef struct {
    void *allocation;
    void *data;                 /**< Aligned storage of the first value. */
    const evio_rx_type *type;
    size_t bytes;               /**< The allocation size. */
    size_t count;               /**< The number of live values. */
} evio_rx_buffer;

/**
 * @brief A bounded FIFO of values.
 * @details Storage has one slot more than the capacity, so a value taken by
 * `evio_rx_ring_shift()` stays valid while up to `capacity` new values are pushed.
 * Storage of a run ring is allocated by init, of a heap ring by the first push.
 */
typedef struct {
    void *allocation;
    char *data;
    const evio_rx_type *type;
    size_t bytes;
    size_t slots;               /**< Capacity + 1. */
    size_t head;
    size_t count;
} evio_rx_ring;

/**
 * @brief Validates a type descriptor.
 * @return 0, or EINVAL.
 */
int evio_rx_type_check(const evio_rx_type *type);

/**
 * @brief Copies a value into uninitialized storage.
 * @return 0, or a positive errno leaving no live `dst`.
 */
static inline __evio_nonnull(1, 2, 3)
int evio_rx_value_copy(const evio_rx_type *type, void *dst, const void *src)
{
    if (!type->copy) {
        memcpy(dst, src, type->size);
        return 0;
    }
    int error = type->copy(type->ctx, dst, src);
    if (error) {
        return error > 0 ? error : EINVAL;
    }
    return 0;
}

/** @brief Destroys a live value. */
static inline __evio_nonnull(1, 2)
void evio_rx_value_destroy(const evio_rx_type *type, void *value)
{
    if (type->destroy) {
        type->destroy(type->ctx, value);
    }
}

/**
 * @brief Initializes an empty buffer for `capacity` values.
 * @param root The owning run, or NULL for the heap.
 * @return 0, or EOVERFLOW.
 */
int evio_rx_buffer_init(evio_rx_buffer *buffer, const evio_rx_type *type, size_t capacity,
                        evio_rx_subscription *root);

/**
 * @brief Initializes an empty buffer over storage aligned for `type`.
 * @details Fini destroys the values and leaves the storage to its owner.
 */
static inline __evio_nonnull(1, 2, 3)
void evio_rx_buffer_wrap(evio_rx_buffer *buffer, const evio_rx_type *type, void *storage)
{
    *buffer = (evio_rx_buffer) { .data = storage, .type = type };
}

/**
 * @brief Copies a value into the next free slot.
 * @return 0, or a positive errno of the copy.
 */
static inline __evio_nonnull(1, 2)
int evio_rx_buffer_push(evio_rx_buffer *buffer, const void *value)
{
    void *dst = (char *)buffer->data + (buffer->count * buffer->type->size);
    int error = evio_rx_value_copy(buffer->type, dst, value);
    if (error) {
        return error;
    }
    buffer->count++;
    return 0;
}

/** @brief Destroys all values. */
static inline __evio_nonnull(1)
void evio_rx_buffer_clear(evio_rx_buffer *buffer)
{
    const evio_rx_type *type = buffer->type;
    if (buffer->count && !type->destroy) {
        buffer->count = 0;
    }
    while (buffer->count) {
        void *value = (char *)buffer->data + (--buffer->count * type->size);
        evio_rx_value_destroy(type, value);
    }
}

/** @brief Destroys all values and releases the storage. */
void evio_rx_buffer_fini(evio_rx_buffer *buffer, evio_rx_subscription *root);

/**
 * @brief Checks that a ring of `capacity` values fits in memory.
 * @return 0, EINVAL for zero capacity, or EOVERFLOW.
 */
int evio_rx_ring_check(const evio_rx_type *type, size_t capacity);

/**
 * @brief Initializes an empty ring.
 * @param root The owning run, or NULL for the heap.
 * @return 0, or an error of `evio_rx_ring_check()`.
 */
int evio_rx_ring_init(evio_rx_ring *ring, const evio_rx_type *type, size_t capacity,
                      evio_rx_subscription *root);

/**
 * @brief Copies a value to the back.
 * @return 0, EOVERFLOW when full, or a positive errno of the copy.
 */
int evio_rx_ring_push(evio_rx_ring *ring, const void *value);

/** @brief Returns the front value of a nonempty ring. */
static inline __evio_nonnull(1)
void *evio_rx_ring_front(const evio_rx_ring *ring)
{
    return ring->data + (ring->head * ring->type->size);
}

/** @brief Returns the slot index past the back value of the ring. */
static inline __evio_nonnull(1)
size_t evio_rx_ring_back(const evio_rx_ring *ring)
{
    size_t index = ring->head + ring->count;
    return index < ring->slots ? index : index - ring->slots;
}

/**
 * @brief Removes the front value and passes its ownership to the caller.
 * @details The caller destroys it before the next shift of the same ring.
 * @param ring A nonempty ring.
 * @return The value.
 */
void *evio_rx_ring_shift(evio_rx_ring *ring);

/** @brief Destroys all queued values. */
void evio_rx_ring_clear(evio_rx_ring *ring);

/** @brief Destroys all queued values and releases the storage. */
void evio_rx_ring_fini(evio_rx_ring *ring, evio_rx_subscription *root);
