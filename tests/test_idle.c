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

TEST(test_evio_idle)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_idle idle;
    evio_idle_init(&idle, generic_cb);
    idle.data = &data;
    evio_idle_start(loop, &idle);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_IDLE);

    evio_idle_stop(loop, &idle);
    evio_loop_free(loop);
}

typedef struct {
    evio_idle *watchers[2];
    size_t called;
    evio_mask emask;
} stop_watchers_data;

static void stop_watchers_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    stop_watchers_data *data = base->data;
    evio_idle *self = container_of(base, evio_idle, base);
    evio_idle *other = self == data->watchers[0] ? data->watchers[1] : data->watchers[0];
    ++data->called;
    data->emask = emask;
    assert_true(other->base.pending);
    evio_idle_stop(loop, other);
    evio_free(other);
    evio_idle_stop(loop, self);
    evio_free(self);
}

TEST(test_evio_idle_stop_and_free_during_dispatch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    stop_watchers_data data = { 0 };
    for (size_t i = 0; i < 2; ++i) {
        data.watchers[i] = evio_calloc(1, sizeof(*data.watchers[i]));
        evio_idle_init(data.watchers[i], stop_watchers_cb);
        data.watchers[i]->data = &data;
    }
    generic_cb_data survivor_data = { 0 };
    evio_idle survivor;
    evio_idle_init(&survivor, generic_cb);
    survivor.data = &survivor_data;
    evio_idle_start(loop, data.watchers[0]);
    evio_idle_start(loop, &survivor);
    evio_idle_start(loop, data.watchers[1]);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_IDLE);
    assert_int_equal(survivor_data.called, 1);
    assert_int_equal(survivor_data.emask, EVIO_IDLE);
    assert_int_equal(evio_pending_count(loop), 0);
    assert_int_equal(evio_refcount(loop), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(survivor_data.called, 2);
    assert_int_equal(evio_pending_count(loop), 0);
    evio_idle_stop(loop, &survivor);
    evio_loop_free(loop);
}

typedef struct {
    evio_idle idle;
    generic_cb_data data;
} idle_watcher_data;

typedef struct {
    idle_watcher_data *watchers;
    size_t count;
    size_t called;
    evio_mask emask;
} start_watchers_data;

static void start_watchers_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    start_watchers_data *data = base->data;
    ++data->called;
    data->emask = emask;
    evio_idle_stop(loop, container_of(base, evio_idle, base));
    for (size_t i = 0; i < data->count; ++i) {
        evio_idle_start(loop, &data->watchers[i].idle);
    }
}

TEST(test_evio_idle_start_during_dispatch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    generic_cb_data survivor_data = { 0 };
    evio_idle survivor;
    evio_idle_init(&survivor, generic_cb);
    survivor.data = &survivor_data;
    evio_idle_start(loop, &survivor);
    start_watchers_data data = { 0 };
    evio_idle idle;
    evio_idle_init(&idle, start_watchers_cb);
    idle.data = &data;
    evio_idle_start(loop, &idle);
    size_t capacity = loop->idle.total;
    data.count = capacity + 1;
    data.watchers = evio_calloc(data.count, sizeof(*data.watchers));
    for (size_t i = 0; i < data.count; ++i) {
        evio_idle_init(&data.watchers[i].idle, generic_cb);
        data.watchers[i].idle.data = &data.watchers[i].data;
    }

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_IDLE);
    assert_int_equal(survivor_data.called, 1);
    assert_int_equal(survivor_data.emask, EVIO_IDLE);
    assert_true(loop->idle.total > capacity);
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
        assert_int_equal(data.watchers[i].data.emask, EVIO_IDLE);
        evio_idle_stop(loop, &data.watchers[i].idle);
    }
    evio_free(data.watchers);
    evio_idle_stop(loop, &survivor);
    evio_loop_free(loop);
}

static void local_timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *counter = base->data;
    ++(*counter);
}

TEST(test_evio_idle_with_pending)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_idle idle;
    evio_idle_init(&idle, generic_cb);
    idle.data = &data;
    evio_idle_start(loop, &idle);

    evio_timer tm;
    evio_timer_init(&tm, local_timer_cb, 0);
    evio_timer_start(loop, &tm, 0);

    size_t counter = 0;
    tm.data = &counter;

    // Timer fired; idle watcher does not fire.
    evio_run(loop, EVIO_RUN_ONCE);

    assert_int_equal(counter, 1);
    assert_int_equal(data.called, 0);

    // Timer stopped; idle watcher fires.
    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(counter, 1); // Unchanged
    assert_int_equal(data.called, 1); // Idle fires

    evio_idle_stop(loop, &idle);
    evio_loop_free(loop);
}
