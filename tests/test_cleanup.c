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

typedef struct {
    evio_cleanup *watchers[2];
    size_t called;
    evio_mask emask;
} stop_watchers_data;

static void stop_watchers_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    stop_watchers_data *data = base->data;
    evio_cleanup *self = container_of(base, evio_cleanup, base);
    evio_cleanup *other = self == data->watchers[0] ? data->watchers[1] : data->watchers[0];
    ++data->called;
    data->emask = emask;
    assert_true(other->base.pending);
    evio_cleanup_stop(loop, other);
    evio_free(other);
    evio_cleanup_stop(loop, self);
    evio_free(self);
}

TEST(test_evio_cleanup_stop_and_free_during_dispatch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    stop_watchers_data data = { 0 };
    for (size_t i = 0; i < 2; ++i) {
        data.watchers[i] = evio_calloc(1, sizeof(*data.watchers[i]));
        evio_cleanup_init(data.watchers[i], stop_watchers_cb);
        data.watchers[i]->data = &data;
    }
    generic_cb_data survivor_data = { 0 };
    evio_cleanup survivor;
    evio_cleanup_init(&survivor, generic_cb);
    survivor.data = &survivor_data;
    evio_cleanup_start(loop, data.watchers[0]);
    evio_cleanup_start(loop, &survivor);
    evio_cleanup_start(loop, data.watchers[1]);

    evio_loop_free(loop);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_CLEANUP);
    assert_int_equal(survivor_data.called, 1);
    assert_int_equal(survivor_data.emask, EVIO_CLEANUP);
}
