#include "test.h"

typedef struct {
    size_t called;
    evio_mask emask;
} generic_cb_data;

static void generic_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    generic_cb_data *data = base->data;
    data->called++;
    data->emask = emask;
}

TEST(test_evio_cleanup)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_cleanup cleanup;
    evio_cleanup_init(&cleanup, generic_cb);
    cleanup.data = &data;

    evio_cleanup_start(loop, &cleanup);
    assert_true(cleanup.base.active);

    assert_int_equal(data.called, 0);
    evio_loop_free(loop);
    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_CLEANUP);
}

TEST(test_evio_cleanup_stopped)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_cleanup cleanup;
    evio_cleanup_init(&cleanup, generic_cb);
    cleanup.data = &data;
    evio_cleanup_start(loop, &cleanup);
    assert_true(cleanup.base.active);

    evio_cleanup_stop(loop, &cleanup);
    assert_false(cleanup.base.active);

    // Stop again, no-op.
    evio_cleanup_stop(loop, &cleanup);
    assert_false(cleanup.base.active);

    evio_loop_free(loop);
    // Callback does not have been called.
    assert_int_equal(data.called, 0);
}
