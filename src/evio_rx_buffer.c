#include <errno.h>

#include "evio_rx_buffer.h"
#include "evio_alloc.h"

int evio_rx_type_check(const evio_rx_type *type)
{
    if (!type || !type->size || !type->alignment ||
        (type->alignment & (type->alignment - 1)) ||
        type->size % type->alignment || (!type->copy && type->destroy)) {
        return EINVAL;
    }
    return 0;
}

static int storage_check(const evio_rx_type *type, size_t count)
{
    if (count > (SIZE_MAX - (type->alignment - 1)) / type->size) {
        return EOVERFLOW;
    }
    return 0;
}

static void *storage_alloc(const evio_rx_type *type, size_t count, size_t *bytes,
                           evio_rx_subscription *root)
{
    *bytes = count * type->size;
    if (type->alignment > alignof(max_align_t)) {
        *bytes += type->alignment - 1;
    }
    return root ? evio_rx_alloc(root, *bytes) : evio_malloc(*bytes);
}

static void *storage_data(const evio_rx_type *type, void *allocation)
{
    size_t padding = -(uintptr_t)allocation & (type->alignment - 1);
    return (char *)allocation + padding;
}

static void storage_free(void *allocation, size_t bytes, evio_rx_subscription *root)
{
    if (root) {
        evio_rx_free(root, allocation, bytes);
        return;
    }
    evio_free(allocation);
}

int evio_rx_buffer_init(evio_rx_buffer *buffer, const evio_rx_type *type, size_t capacity,
                        evio_rx_subscription *root)
{
    *buffer = (evio_rx_buffer) { .type = type };
    if (!capacity) {
        return 0;
    }
    int error = storage_check(type, capacity);
    if (error) {
        return error;
    }
    buffer->allocation = storage_alloc(type, capacity, &buffer->bytes, root);
    buffer->data = storage_data(type, buffer->allocation);
    return 0;
}

void evio_rx_buffer_fini(evio_rx_buffer *buffer, evio_rx_subscription *root)
{
    evio_rx_buffer_clear(buffer);
    storage_free(buffer->allocation, buffer->bytes, root);
    *buffer = (evio_rx_buffer) {0};
}

int evio_rx_ring_check(const evio_rx_type *type, size_t capacity)
{
    if (!capacity) {
        return EINVAL;
    }
    if (capacity == SIZE_MAX) {
        return EOVERFLOW;
    }
    return storage_check(type, capacity + 1);
}

int evio_rx_ring_init(evio_rx_ring *ring, const evio_rx_type *type, size_t capacity,
                      evio_rx_subscription *root)
{
    int error = evio_rx_ring_check(type, capacity);
    if (error) {
        return error;
    }
    *ring = (evio_rx_ring) { .type = type, .slots = capacity + 1 };
    if (root) {
        ring->allocation = storage_alloc(type, ring->slots, &ring->bytes, root);
        ring->data = storage_data(type, ring->allocation);
    }
    return 0;
}

int evio_rx_ring_push(evio_rx_ring *ring, const void *value)
{
    if (ring->count == ring->slots - 1) {
        return EOVERFLOW;
    }
    if (!ring->allocation) {
        ring->allocation = storage_alloc(ring->type, ring->slots, &ring->bytes, NULL);
        ring->data = storage_data(ring->type, ring->allocation);
    }
    // The slot is reserved before the copy hook, which may push to the same ring.
    size_t index = evio_rx_ring_back(ring);
    size_t position = ring->count++;
    int error = evio_rx_value_copy(ring->type, ring->data + (index * ring->type->size), value);
    if (!error) {
        return 0;
    }
    // A failed push terminates the owner, which discards values pushed by the hook.
    while (ring->count > position + 1) {
        ring->count--;
        evio_rx_value_destroy(ring->type, ring->data + (evio_rx_ring_back(ring) * ring->type->size));
    }
    ring->count = position;
    return error;
}

void *evio_rx_ring_shift(evio_rx_ring *ring)
{
    void *value = evio_rx_ring_front(ring);
    if (++ring->head == ring->slots) {
        ring->head = 0;
    }
    ring->count--;
    return value;
}

void evio_rx_ring_clear(evio_rx_ring *ring)
{
    while (ring->count) {
        evio_rx_value_destroy(ring->type, evio_rx_ring_shift(ring));
    }
}

void evio_rx_ring_fini(evio_rx_ring *ring, evio_rx_subscription *root)
{
    evio_rx_ring_clear(ring);
    if (ring->allocation) {
        storage_free(ring->allocation, ring->bytes, root);
    }
    *ring = (evio_rx_ring) {0};
}
