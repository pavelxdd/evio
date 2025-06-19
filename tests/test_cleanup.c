#include "test.h"

TEST(test_evio_cleanup)
{
    // Part 1: Test that a started watcher is called on loop free.
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_cleanup cleanup;
    evio_cleanup_init(&cleanup, generic_cb);
    evio_cleanup_start(loop, &cleanup);
    assert_true(cleanup.base.active);

    assert_int_equal(generic_cb_called, 0);
    evio_loop_free(loop);
    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb_emask, EVIO_CLEANUP);

    // Part 2: Test that a stopped watcher is NOT called.
    reset_cb_state();
    loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_cleanup_init(&cleanup, generic_cb);
    evio_cleanup_start(loop, &cleanup);
    assert_true(cleanup.base.active);

    evio_cleanup_stop(loop, &cleanup);
    assert_false(cleanup.base.active);

    // Stop again, should be a no-op.
    evio_cleanup_stop(loop, &cleanup);
    assert_false(cleanup.base.active);

    evio_loop_free(loop);
    // Callback should not have been called.
    assert_int_equal(generic_cb_called, 0);
}
