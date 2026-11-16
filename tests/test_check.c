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

TEST(test_evio_check)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_check check;
    evio_check_init(&check, generic_cb);
    check.data = &data;
    evio_check_start(loop, &check);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_CHECK);

    evio_check_stop(loop, &check);
    evio_loop_free(loop);
}

typedef struct {
    evio_check *watchers[2];
    size_t called;
    evio_mask emask;
} stop_watchers_data;

static void stop_watchers_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    stop_watchers_data *data = base->data;
    evio_check *self = container_of(base, evio_check, base);
    evio_check *other = self == data->watchers[0] ? data->watchers[1] : data->watchers[0];
    ++data->called;
    data->emask = emask;
    assert_true(other->base.pending);
    evio_check_stop(loop, other);
    evio_free(other);
    evio_check_stop(loop, self);
    evio_free(self);
}

TEST(test_evio_check_stop_and_free_during_dispatch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    stop_watchers_data data = { 0 };
    for (size_t i = 0; i < 2; ++i) {
        data.watchers[i] = evio_calloc(1, sizeof(*data.watchers[i]));
        evio_check_init(data.watchers[i], stop_watchers_cb);
        data.watchers[i]->data = &data;
    }
    generic_cb_data survivor_data = { 0 };
    evio_check survivor;
    evio_check_init(&survivor, generic_cb);
    survivor.data = &survivor_data;
    evio_check_start(loop, data.watchers[0]);
    evio_check_start(loop, &survivor);
    evio_check_start(loop, data.watchers[1]);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_CHECK);
    assert_int_equal(survivor_data.called, 1);
    assert_int_equal(survivor_data.emask, EVIO_CHECK);
    assert_int_equal(evio_pending_count(loop), 0);
    assert_int_equal(evio_refcount(loop), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(survivor_data.called, 2);
    assert_int_equal(evio_pending_count(loop), 0);
    evio_check_stop(loop, &survivor);
    evio_loop_free(loop);
}

typedef struct {
    evio_check check;
    generic_cb_data data;
} check_watcher_data;

typedef struct {
    check_watcher_data *watchers;
    size_t count;
    size_t called;
    evio_mask emask;
} start_watchers_data;

static void start_watchers_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    start_watchers_data *data = base->data;
    ++data->called;
    data->emask = emask;
    evio_check_stop(loop, container_of(base, evio_check, base));
    for (size_t i = 0; i < data->count; ++i) {
        evio_check_start(loop, &data->watchers[i].check);
    }
}

TEST(test_evio_check_start_during_dispatch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    generic_cb_data survivor_data = { 0 };
    evio_check survivor;
    evio_check_init(&survivor, generic_cb);
    survivor.data = &survivor_data;
    evio_check_start(loop, &survivor);
    start_watchers_data data = { 0 };
    evio_check check;
    evio_check_init(&check, start_watchers_cb);
    check.data = &data;
    evio_check_start(loop, &check);
    size_t capacity = loop->check.total;
    data.count = capacity + 1;
    data.watchers = evio_calloc(data.count, sizeof(*data.watchers));
    for (size_t i = 0; i < data.count; ++i) {
        evio_check_init(&data.watchers[i].check, generic_cb);
        data.watchers[i].check.data = &data.watchers[i].data;
    }

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_CHECK);
    assert_int_equal(survivor_data.called, 1);
    assert_int_equal(survivor_data.emask, EVIO_CHECK);
    assert_true(loop->check.total > capacity);
    assert_int_equal(evio_pending_count(loop), 0);
    assert_int_equal(evio_refcount(loop), data.count + 1);
    for (size_t i = 0; i < data.count; ++i) {
        assert_int_equal(data.watchers[i].data.called, 0);
    }

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(survivor_data.called, 2);
    assert_int_equal(evio_pending_count(loop), 0);
    for (size_t i = 0; i < data.count; ++i) {
        assert_int_equal(data.watchers[i].data.called, 1);
        assert_int_equal(data.watchers[i].data.emask, EVIO_CHECK);
        evio_check_stop(loop, &data.watchers[i].check);
    }
    evio_free(data.watchers);
    evio_check_stop(loop, &survivor);
    evio_loop_free(loop);
}
