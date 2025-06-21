#include "test.h"

// GCOVR_EXCL_START
static void dummy_cb(evio_loop *loop, evio_base *base, evio_mask emask) {}
// GCOVR_EXCL_STOP

TEST(test_evio_heap_sift)
{
    // Test for evio_heap_down to cover both left and right child comparisons
    evio_node heap[3];
    evio_base b[3];
    for (int i = 0; i < 3; ++i) {
        evio_init(&b[i], dummy_cb);
    }

    // Case 1: left child is smaller
    heap[0] = (evio_node) {
        .base = &b[0], .time = 100
    };
    heap[1] = (evio_node) {
        .base = &b[1], .time = 20
    }; // left child
    heap[2] = (evio_node) {
        .base = &b[2], .time = 30
    }; // right child
    for (int i = 0; i < 3; ++i) {
        heap[i].base->active = i + 1;
    }

    evio_heap_down(heap, 0, 3);
    assert_int_equal(heap[0].time, 20); // smallest sifts up

    // Case 2: right child is smaller
    heap[0] = (evio_node) {
        .base = &b[0], .time = 100
    };
    heap[1] = (evio_node) {
        .base = &b[1], .time = 30
    }; // left child
    heap[2] = (evio_node) {
        .base = &b[2], .time = 20
    }; // right child
    for (int i = 0; i < 3; ++i) {
        heap[i].base->active = i + 1;
    }

    evio_heap_down(heap, 0, 3);
    assert_int_equal(heap[0].time, 20); // smallest sifts up
}

TEST(test_evio_heap_adjust_up)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm1, tm2, tm3;
    evio_timer_init(&tm1, dummy_cb, 0);
    evio_timer_init(&tm2, dummy_cb, 0);
    evio_timer_init(&tm3, dummy_cb, 0);

    // Start timers to build a heap
    evio_timer_start(loop, &tm1, 100); // root
    evio_timer_start(loop, &tm2, 200); // child
    evio_timer_start(loop, &tm3, 300); // child

    assert_int_equal(loop->timer.count, 3);
    assert_true(tm3.base.active > 0);

    // Manually change a child's time to be smaller than the root.
    // The watcher's `active` field holds its 1-based index in the heap.
    size_t index = tm3.base.active - 1;
    assert_ptr_equal(loop->timer.ptr[index].base, &tm3.base);
    loop->timer.ptr[index].time = 50;

    // Now adjust it
    evio_heap_adjust(loop->timer.ptr, index, loop->timer.count);

    // After adjustment, t3 should be the new root
    assert_ptr_equal(loop->timer.ptr[0].base, &tm3.base);

    evio_timer_stop(loop, &tm1);
    evio_timer_stop(loop, &tm2);
    evio_timer_stop(loop, &tm3);
    evio_loop_free(loop);
}

static bool is_min_heap(const evio_node *heap, size_t count)
{
    // For every node with children, check if it's smaller than them.
    for (size_t i = 0; i < count / 2; ++i) {
        size_t l = 2 * i + 1;
        size_t r = l + 1;
        size_t m = l;

        if (r < count && heap[r].time < heap[l].time) {
            m = r;
        }

        if (heap[i].time > heap[m].time) {
            return false;
        }
    }
    return true;
}

TEST(test_is_min_heap_coverage)
{
    evio_node heap[4];
    evio_base b[4];
    for (int i = 0; i < 4; ++i) {
        evio_init(&b[i], dummy_cb);
    }

    // Case 1: Valid heap where right child is smaller than left.
    // Covers path: (r < count) is true, (heap[r].time < heap[l].time) is true.
    heap[0] = (evio_node) {
        .base = &b[0],
        .time = 10,
    };
    heap[1] = (evio_node) {
        .base = &b[1],
        .time = 30,
    }; // left child
    heap[2] = (evio_node) {
        .base = &b[2],
        .time = 20,
    }; // right child
    assert_true(is_min_heap(heap, 3));

    // Case 2: Valid heap where left child is smaller than right.
    // Covers path: (r < count) is true, (heap[r].time < heap[l].time) is false.
    heap[0] = (evio_node) {
        .base = &b[0],
        .time = 10,
    };
    heap[1] = (evio_node) {
        .base = &b[1],
        .time = 20,
    }; // left child
    heap[2] = (evio_node) {
        .base = &b[2],
        .time = 30,
    }; // right child
    assert_true(is_min_heap(heap, 3));

    // Case 3: Valid heap with a node that has only a left child.
    // Covers path: (r < count) is false.
    heap[0] = (evio_node) {
        .base = &b[0],
        .time = 10,
    };
    heap[1] = (evio_node) {
        .base = &b[1],
        .time = 20,
    };
    assert_true(is_min_heap(heap, 2));

    // Case 4: Not a min-heap.
    // Covers the `return false` path.
    heap[0] = (evio_node) {
        .base = &b[0],
        .time = 30,
    };
    heap[1] = (evio_node) {
        .base = &b[1],
        .time = 20,
    };
    assert_false(is_min_heap(heap, 2));

    // Case 5: Heap with zero elements.
    // Hits the `for` loop condition being false on the first check.
    assert_true(is_min_heap(heap, 0));

    // Case 6: Heap with only one element.
    // Also hits the `for` loop condition being false on the first check.
    heap[0] = (evio_node) {
        .base = &b[0],
        .time = 10,
    };
    assert_true(is_min_heap(heap, 1));
}

TEST(test_evio_heap_adjust_down)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm[5];
    for (int i = 0; i < 5; ++i) {
        evio_timer_init(&tm[i], dummy_cb, 0);
    }

    // Build a heap that allows for a clear sift-down scenario.
    evio_timer_start(loop, &tm[0], 100); // root
    evio_timer_start(loop, &tm[1], 200); // parent of 300, 400
    evio_timer_start(loop, &tm[2], 110);
    evio_timer_start(loop, &tm[3], 300);
    evio_timer_start(loop, &tm[4], 400);

    assert_int_equal(loop->timer.count, 5);
    assert_true(is_min_heap(loop->timer.ptr, loop->timer.count));

    // Find tm[1] (time loop->time + 200).
    size_t index = tm[1].base.active - 1;
    assert_ptr_equal(loop->timer.ptr[index].base, &tm[1].base);
    assert_true(index > 0);

    // Increase its time so it's larger than its children, invalidating the heap.
    loop->timer.ptr[index].time = evio_get_time(loop) + 500;
    assert_false(is_min_heap(loop->timer.ptr, loop->timer.count));

    // Adjusting it should trigger the `else` branch of evio_heap_adjust,
    // call evio_heap_down, and restore the heap property.
    evio_heap_adjust(loop->timer.ptr, index, loop->timer.count);
    assert_true(is_min_heap(loop->timer.ptr, loop->timer.count));

    // The root should still be tm[0].
    assert_ptr_equal(loop->timer.ptr[0].base, &tm[0].base);

    for (int i = 0; i < 5; ++i) {
        evio_timer_stop(loop, &tm[i]);
    }
    evio_loop_free(loop);
}
