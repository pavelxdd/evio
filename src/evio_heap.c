#include "evio_core.h"
#include "evio_heap.h"

/**
 * @brief Calculates the parent index of a node in the heap.
 * @param i The child node's index.
 * @return The parent node's index.
 */
static inline __evio_nodiscard
size_t evio_heap_parent(size_t i)
{
    return (i - 1) >> 1;
}

/**
 * @brief Calculates the left child index of a node in the heap.
 * @param i The parent node's index.
 * @return The left child's index.
 */
static inline __evio_nodiscard
size_t evio_heap_child(size_t i)
{
    return (i << 1) + 1;
}

void evio_heap_up(evio_node *heap, size_t index)
{
    evio_node node = heap[index];

    while (index) {
        size_t p = evio_heap_parent(index);
        if (heap[p].time <= node.time) {
            break;
        }

        heap[index] = heap[p];
        heap[index].base->active = index + 1;

        index = p;
    }

    heap[index] = node;
    heap[index].base->active = index + 1;
}

void evio_heap_down(evio_node *heap, size_t index, size_t count)
{
    evio_node node = heap[index];

    for (;;) {
        size_t l = evio_heap_child(index);
        if (l >= count) {
            break;
        }

        size_t r = l + 1;
        size_t m = (r < count && heap[r].time < heap[l].time) ? r : l;

        if (node.time <= heap[m].time) {
            break;
        }

        heap[index] = heap[m];
        heap[index].base->active = index + 1;

        index = m;
    }

    heap[index] = node;
    heap[index].base->active = index + 1;
}

void evio_heap_adjust(evio_node *heap, size_t index, size_t count)
{
    if (index && heap[index].time <= heap[evio_heap_parent(index)].time) {
        evio_heap_up(heap, index);
    } else {
        evio_heap_down(heap, index, count);
    }
}
