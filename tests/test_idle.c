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
