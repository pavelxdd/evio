#include "test.h"

TEST(test_evio_idle)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_idle idle;
    evio_idle_init(&idle, generic_cb);
    evio_idle_start(loop, &idle);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb_emask, EVIO_IDLE);

    evio_idle_stop(loop, &idle);
    evio_loop_free(loop);
}

static size_t timer_cb_called = 0;

static void local_timer_cb(evio_loop *loop, evio_base *w, evio_mask emask)
{
    timer_cb_called++;
}

TEST(test_evio_idle_with_pending)
{
    reset_cb_state();
    timer_cb_called = 0;

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_idle idle;
    evio_idle_init(&idle, generic_cb);
    evio_idle_start(loop, &idle);

    evio_timer tm;
    evio_timer_init(&tm, local_timer_cb, 0);
    evio_timer_start(loop, &tm, 0);

    // Run once. The timer will fire. Because a timer event was pending,
    // the idle watcher should NOT fire.
    evio_run(loop, EVIO_RUN_ONCE);

    assert_int_equal(timer_cb_called, 1);
    assert_int_equal(generic_cb_called, 0);

    // Run again. Now the timer is stopped, and no events are pending.
    // The idle watcher should fire.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(timer_cb_called, 1);   // Unchanged
    assert_int_equal(generic_cb_called, 1); // Idle fires

    evio_idle_stop(loop, &idle);
    evio_loop_free(loop);
}
