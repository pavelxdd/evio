#include "test.h"

TEST(test_evio_list_double_start)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare p;
    evio_prepare_init(&p, generic_cb);
    evio_prepare_start(loop, &p);
    evio_prepare_start(loop, &p); // double start
    assert_int_equal(evio_refcount(loop), 1);
    evio_prepare_stop(loop, &p);
    evio_prepare_stop(loop, &p); // double stop

    evio_check c;
    evio_check_init(&c, generic_cb);
    evio_check_start(loop, &c);
    evio_check_start(loop, &c);
    assert_int_equal(evio_refcount(loop), 1);
    evio_check_stop(loop, &c);
    evio_check_stop(loop, &c);

    evio_idle i;
    evio_idle_init(&i, generic_cb);
    evio_idle_start(loop, &i);
    evio_idle_start(loop, &i);
    assert_int_equal(evio_refcount(loop), 1);
    evio_idle_stop(loop, &i);
    evio_idle_stop(loop, &i);

    evio_cleanup cl;
    evio_cleanup_init(&cl, generic_cb);
    evio_cleanup_start(loop, &cl);
    evio_cleanup_start(loop, &cl);
    // no refcount for cleanup watchers
    evio_cleanup_stop(loop, &cl);
    evio_cleanup_stop(loop, &cl);

    evio_loop_free(loop);
}

TEST(test_evio_list_stop_middle)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare p1, p2, p3;
    evio_prepare_init(&p1, generic_cb);
    evio_prepare_init(&p2, generic_cb);
    evio_prepare_init(&p3, generic_cb);

    evio_prepare_start(loop, &p1);
    evio_prepare_start(loop, &p2);
    evio_prepare_start(loop, &p3);

    assert_int_equal(loop->prepare.count, 3);
    assert_int_equal(p1.base.active, 1);
    assert_int_equal(p2.base.active, 2);
    assert_int_equal(p3.base.active, 3);

    // Stop the middle one
    evio_prepare_stop(loop, &p2);

    assert_int_equal(loop->prepare.count, 2);
    assert_false(p2.base.active);
    // p3 should have been moved to p2's slot
    assert_ptr_equal(loop->prepare.ptr[1], &p3.base);
    assert_int_equal(p3.base.active, 2);

    evio_prepare_stop(loop, &p1);
    evio_prepare_stop(loop, &p3);

    assert_int_equal(loop->prepare.count, 0);

    evio_loop_free(loop);
}

TEST(test_evio_list_resize_assert_zero_size)
{
    expect_assert_failure({
        size_t total = 0;
        evio_list_resize(NULL, 0, 1, &total);
        fail(); // GCOVR_EXCL_LINE
    });
}

TEST(test_evio_list_resize_assert_overflow)
{
    expect_assert_failure({
        size_t total = 0;
        evio_list_resize(NULL, 2, (SIZE_MAX / 2) + 1, &total);
        fail(); // GCOVR_EXCL_LINE
    });
}

TEST(test_evio_list_resize_assert_null_ptr)
{
    expect_assert_failure({
        size_t total = 1;
        evio_list_resize(NULL, 1, 0, &total);
        fail(); // GCOVR_EXCL_LINE
    });
}

TEST(test_evio_list_resize_count_zero)
{
    // Test resizing to zero when the buffer is already allocated.
    // This should take the `if (*total >= count)` path and do nothing.
    size_t total = 10;
    char *p1 = evio_malloc(total);
    assert_non_null(p1);

    char *p2 = evio_list_resize(p1, 1, 0, &total);
    assert_ptr_equal(p1, p2);
    // Total should be unchanged.
    assert_int_equal(total, 10);
    evio_free(p1);
}

TEST(test_evio_list_resize_no_realloc)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare p1, p2;
    evio_prepare_init(&p1, generic_cb);
    evio_prepare_init(&p2, generic_cb);

    // First start will allocate memory.
    evio_prepare_start(loop, &p1);
    assert_int_equal(loop->prepare.count, 1);
    assert_true(loop->prepare.total >= 1);

    // Second start should not reallocate because total is likely sufficient.
    // This will hit the uncovered branch in evio_list_resize.
    evio_prepare_start(loop, &p2);
    assert_int_equal(loop->prepare.count, 2);

    evio_prepare_stop(loop, &p1);
    evio_prepare_stop(loop, &p2);
    evio_loop_free(loop);
}
