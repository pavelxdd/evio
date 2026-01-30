#include "test.h"

// GCOVR_EXCL_START
static void dummy_cb(evio_loop *loop, evio_base *base, evio_mask emask) {}
// GCOVR_EXCL_STOP

TEST(test_evio_list_double_start_stop)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, dummy_cb);

    evio_prepare_start(loop, &prepare);
    assert_int_equal(loop->prepare.count, 1);
    assert_int_equal(evio_refcount(loop), 1);

    evio_prepare_start(loop, &prepare); // double start no-op
    assert_int_equal(loop->prepare.count, 1);
    assert_int_equal(evio_refcount(loop), 1);

    evio_prepare_stop(loop, &prepare);
    assert_int_equal(loop->prepare.count, 0);
    assert_int_equal(evio_refcount(loop), 0);

    evio_prepare_stop(loop, &prepare); // double stop no-op
    assert_int_equal(loop->prepare.count, 0);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_list_stop_middle)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare[3];
    for (size_t i = 0; i < 3; ++i) {
        evio_prepare_init(&prepare[i], dummy_cb);
        evio_prepare_start(loop, &prepare[i]);
    }

    assert_int_equal(loop->prepare.count, 3);

    assert_int_equal(prepare[0].base.active, 1);
    assert_int_equal(prepare[1].base.active, 2);
    assert_int_equal(prepare[2].base.active, 3);

    // Stop the middle one
    evio_prepare_stop(loop, &prepare[1]);

    assert_int_equal(loop->prepare.count, 2);
    assert_false(prepare[1].base.active);
    // prepare[2] was moved to prepare[1]'s slot
    assert_ptr_equal(loop->prepare.ptr[1], &prepare[2].base);
    assert_int_equal(prepare[2].base.active, 2);

    evio_prepare_stop(loop, &prepare[0]);
    evio_prepare_stop(loop, &prepare[2]);

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
    size_t total = 10;
    char *p1 = evio_malloc(total);
    assert_non_null(p1);

    char *p2 = evio_list_resize(p1, 1, 0, &total);
    assert_ptr_equal(p1, p2);
    assert_int_equal(total, 10);
    evio_free(p1);
}

TEST(test_evio_list_resize_no_realloc)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare1;
    evio_prepare_init(&prepare1, dummy_cb);

    evio_prepare prepare2;
    evio_prepare_init(&prepare2, dummy_cb);

    // First start will allocate memory.
    evio_prepare_start(loop, &prepare1);
    assert_int_equal(loop->prepare.count, 1);
    assert_true(loop->prepare.total >= 1);

    // Second start should not grow the list.
    evio_prepare_start(loop, &prepare2);
    assert_int_equal(loop->prepare.count, 2);

    evio_prepare_stop(loop, &prepare1);
    evio_prepare_stop(loop, &prepare2);
    evio_loop_free(loop);
}

TEST(test_evio_list_resize_power_of_two_exact)
{
    // count is power-of-two => total stays exact.
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Start exactly 4 watchers (4 is a power of 2)
    evio_prepare prepare[4];
    for (size_t i = 0; i < 4; ++i) {
        evio_prepare_init(&prepare[i], dummy_cb);
        evio_prepare_start(loop, &prepare[i]);
    }

    // Count = 4, which is a power of 2. Total is exactly 4, not 8.
    assert_int_equal(loop->prepare.count, 4);
    assert_int_equal(loop->prepare.total, 4);

    // Add one more (count = 5, not a power of 2)
    evio_prepare prepare5;
    evio_prepare_init(&prepare5, dummy_cb);
    evio_prepare_start(loop, &prepare5);

    // Now count = 5, next power of 2 is 8
    assert_int_equal(loop->prepare.count, 5);
    assert_int_equal(loop->prepare.total, 8);

    for (size_t i = 0; i < 4; ++i) {
        evio_prepare_stop(loop, &prepare[i]);
    }
    evio_prepare_stop(loop, &prepare5);
    evio_loop_free(loop);
}

TEST(test_evio_list_resize_power_of_two_1)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, dummy_cb);
    evio_prepare_start(loop, &prepare);

    assert_int_equal(loop->prepare.count, 1);
    assert_int_equal(loop->prepare.total, 2);

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

TEST(test_evio_list_resize_power_of_two_2)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare[2];
    for (size_t i = 0; i < 2; ++i) {
        evio_prepare_init(&prepare[i], dummy_cb);
        evio_prepare_start(loop, &prepare[i]);
    }

    // Count = 2, which is a power of 2. Total is 2.
    assert_int_equal(loop->prepare.count, 2);
    assert_int_equal(loop->prepare.total, 2);

    for (size_t i = 0; i < 2; ++i) {
        evio_prepare_stop(loop, &prepare[i]);
    }
    evio_loop_free(loop);
}

TEST(test_evio_list_resize_zero_from_zero)
{
    size_t total = 0;
    void *ptr = NULL;

    void *result = evio_list_resize(ptr, sizeof(int), 0, &total);

    assert_non_null(result);
    assert_int_equal(total, 2);

    evio_free(result);
}

TEST(test_evio_list_resize_one_from_zero)
{
    size_t total = 0;
    void *ptr = NULL;

    void *result = evio_list_resize(ptr, sizeof(int), 1, &total);

    assert_non_null(result);
    assert_int_equal(total, 2);

    evio_free(result);
}
