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

TEST(test_evio_prepare)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, generic_cb);
    prepare.data = &data;
    evio_prepare_start(loop, &prepare);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_PREPARE);

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

typedef struct {
    evio_prepare *watchers[2];
    size_t called;
    evio_mask emask;
} stop_watchers_data;

static void stop_watchers_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    stop_watchers_data *data = base->data;
    evio_prepare *self = container_of(base, evio_prepare, base);
    evio_prepare *other = self == data->watchers[0] ? data->watchers[1] : data->watchers[0];
    ++data->called;
    data->emask = emask;
    assert_true(other->base.pending);
    evio_prepare_stop(loop, other);
    evio_free(other);
    evio_prepare_stop(loop, self);
    evio_free(self);
}

TEST(test_evio_prepare_stop_and_free_during_dispatch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    stop_watchers_data data = { 0 };
    for (size_t i = 0; i < 2; ++i) {
        data.watchers[i] = evio_calloc(1, sizeof(*data.watchers[i]));
        evio_prepare_init(data.watchers[i], stop_watchers_cb);
        data.watchers[i]->data = &data;
    }
    generic_cb_data survivor_data = { 0 };
    evio_prepare survivor;
    evio_prepare_init(&survivor, generic_cb);
    survivor.data = &survivor_data;
    evio_prepare_start(loop, data.watchers[0]);
    evio_prepare_start(loop, &survivor);
    evio_prepare_start(loop, data.watchers[1]);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_PREPARE);
    assert_int_equal(survivor_data.called, 1);
    assert_int_equal(survivor_data.emask, EVIO_PREPARE);
    assert_int_equal(evio_pending_count(loop), 0);
    assert_int_equal(evio_refcount(loop), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(survivor_data.called, 2);
    assert_int_equal(evio_pending_count(loop), 0);
    evio_prepare_stop(loop, &survivor);
    evio_loop_free(loop);
}

typedef struct {
    evio_prepare prepare;
    generic_cb_data data;
} prepare_watcher_data;

typedef struct {
    prepare_watcher_data *watchers;
    size_t count;
    size_t called;
    evio_mask emask;
} start_watchers_data;

static void start_watchers_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    start_watchers_data *data = base->data;
    ++data->called;
    data->emask = emask;
    evio_prepare_stop(loop, container_of(base, evio_prepare, base));
    for (size_t i = 0; i < data->count; ++i) {
        evio_prepare_start(loop, &data->watchers[i].prepare);
    }
}

TEST(test_evio_prepare_start_during_dispatch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    generic_cb_data survivor_data = { 0 };
    evio_prepare survivor;
    evio_prepare_init(&survivor, generic_cb);
    survivor.data = &survivor_data;
    evio_prepare_start(loop, &survivor);
    start_watchers_data data = { 0 };
    evio_prepare prepare;
    evio_prepare_init(&prepare, start_watchers_cb);
    prepare.data = &data;
    evio_prepare_start(loop, &prepare);
    size_t capacity = loop->prepare.total;
    data.count = capacity + 1;
    data.watchers = evio_calloc(data.count, sizeof(*data.watchers));
    for (size_t i = 0; i < data.count; ++i) {
        evio_prepare_init(&data.watchers[i].prepare, generic_cb);
        data.watchers[i].prepare.data = &data.watchers[i].data;
    }

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_PREPARE);
    assert_int_equal(survivor_data.called, 1);
    assert_int_equal(survivor_data.emask, EVIO_PREPARE);
    assert_true(loop->prepare.total > capacity);
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
        assert_int_equal(data.watchers[i].data.emask, EVIO_PREPARE);
        evio_prepare_stop(loop, &data.watchers[i].prepare);
    }
    evio_free(data.watchers);
    evio_prepare_stop(loop, &survivor);
    evio_loop_free(loop);
}
