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

// A callback that reads from the fd to clear the event state.
static void read_and_count_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    char buf[1];
    evio_poll *io = container_of(base, evio_poll, base);
    read(io->fd, buf, sizeof(buf));
    generic_cb(loop, base, emask);
}

TEST(test_evio_timeout_huge_ns_diff)
{
    generic_cb_data data1 = { 0 };
    generic_cb_data data2 = { 0 };

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    tm.data = &data1;
    evio_timer_start(loop, &tm, 1); // Start with a dummy timeout

    evio_poll io;
    evio_poll_init(&io, read_and_count_cb, fds[0], EVIO_READ);
    io.data = &data2;
    evio_poll_start(loop, &io);

    assert_int_equal(loop->timer.count, 1);
    loop->timer.ptr[0].time = EVIO_TIME_MAX;

    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_ONCE);

    // Poll watcher fired, timer did not.
    assert_int_equal(data1.called, 0);
    assert_int_equal(data2.called, 1);

    // Cleanup
    evio_timer_stop(loop, &tm);
    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_timeout_huge_ms_diff)
{
    generic_cb_data data1 = { 0 };
    generic_cb_data data2 = { 0 };

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    tm.data = &data1;
    evio_timer_start(loop, &tm, 1); // Start with a dummy timeout

    evio_poll io;
    evio_poll_init(&io, read_and_count_cb, fds[0], EVIO_READ);
    io.data = &data2;
    evio_poll_start(loop, &io);

    assert_int_equal(loop->timer.count, 1);
    loop->timer.ptr[0].time = evio_get_time(loop) +
                              ((EVIO_TIME_C(INT_MAX) + 1) * EVIO_TIME_PER_MSEC);

    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_ONCE);

    // Poll watcher fired, timer did not.
    assert_int_equal(data1.called, 0);
    assert_int_equal(data2.called, 1);

    // Cleanup
    evio_timer_stop(loop, &tm);
    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_timer)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    tm.data = &data;
    evio_timer_start(loop, &tm, 0); // Start with 0 timeout to fire immediately

    assert_int_equal(evio_refcount(loop), 1);
    assert_int_equal(data.called, 0);

    // Double start: no-op
    evio_timer_start(loop, &tm, 0);
    assert_int_equal(evio_refcount(loop), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_TIMER);

    // Timer is stopped now (one-shot)
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_repeat)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 1);
    tm.data = &data;
    evio_timer_start(loop, &tm, 0);

    assert_int_equal(evio_refcount(loop), 1);

    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(data.called, 1);
    assert_int_equal(evio_refcount(loop), 1); // Should still be active

    usleep(20000); // 20ms

    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(data.called, 2);

    evio_timer_stop(loop, &tm);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

static size_t run_until_called_or_timeout(evio_loop *loop, generic_cb_data *data,
                                          size_t loop_limit, int flags)
{
    size_t loops = 0;
    while (data->called == 0 && loops < loop_limit) {
        evio_run(loop, flags);
        loops++;
    }
    return loops;
}

TEST(test_evio_timer_again_and_remaining)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    // repeating timer, 10 seconds
    evio_timer_init(&tm, generic_cb, EVIO_TIME_FROM_SEC(10));
    tm.data = &data;
    // Start with a timeout larger than the coarse clock resolution to make the test stable.
    evio_timer_start(loop, &tm, EVIO_TIME_FROM_MSEC(50));

    assert_true(evio_timer_remaining(loop, &tm) > 0);

    // On a busy CI server, the single run might not be enough due to clock granularity.
    // We loop until the timer fires, with a safety break.
    run_until_called_or_timeout(loop, &data, 10, EVIO_RUN_ONCE);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_TIMER);
    data.called = 0;

    assert_true(evio_timer_remaining(loop, &tm) <= EVIO_TIME_FROM_SEC(10));

    evio_timer_again(loop, &tm);
    assert_int_equal(data.called, 0);

    assert_true(evio_timer_remaining(loop, &tm) <= EVIO_TIME_FROM_SEC(10));

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0); // not fired yet

    evio_timer_stop(loop, &tm);
    assert_int_equal(evio_timer_remaining(loop, &tm), 0);
    assert_int_equal(evio_refcount(loop), 0);

    evio_timer_set(&tm, EVIO_TIME_FROM_MSEC(10));
    evio_timer_again(loop, &tm);
    assert_int_equal(evio_refcount(loop), 1); // Should be restarted
    assert_true(evio_timer_remaining(loop, &tm) > 0);

    evio_timer_stop(loop, &tm);
    evio_loop_free(loop);
}

TEST(test_evio_timer_again_loop_break)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    tm.data = &data;
    // Start with a long timeout that won't fire.
    evio_timer_start(loop, &tm, EVIO_TIME_FROM_SEC(10));

    // Loop times out.
    size_t loops = run_until_called_or_timeout(loop, &data, 5, EVIO_RUN_NOWAIT);
    assert_int_equal(loops, 5);
    assert_int_equal(data.called, 0);

    evio_timer_stop(loop, &tm);
    evio_loop_free(loop);
}

TEST(test_evio_timer_again_one_shot)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0); // one-shot
    tm.data = &data;
    evio_timer_start(loop, &tm, 100);
    assert_true(tm.active);
    assert_int_equal(evio_refcount(loop), 1);

    // 'again' on a non-repeating timer stops it.
    evio_timer_again(loop, &tm);
    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_again_overflow)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, EVIO_TIME_MAX); // large repeat
    tm.data = &data;
    evio_timer_start(loop, &tm, 1);
    assert_true(tm.active);

    // This stops the timer due to overflow check in evio_timer_again.
    evio_timer_again(loop, &tm);
    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_update_overflow)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, EVIO_TIME_MAX); // large repeat
    tm.data = &data;
    evio_timer_start(loop, &tm, 0);                  // fire immediately

    assert_int_equal(evio_refcount(loop), 1);

    // This run will fire the timer. When rescheduling in evio_timer_update,
    // it will hit the overflow check and stop the timer instead of repeating.
    evio_run(loop, EVIO_RUN_ONCE);

    assert_int_equal(data.called, 1);
    assert_int_equal(evio_refcount(loop), 0); // is stopped
    assert_false(tm.active);

    evio_loop_free(loop);
}

TEST(test_evio_timer_overflow_on_reschedule)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 1000);
    tm.data = &data;

    evio_timer_start(loop, &tm, EVIO_TIME_FROM_MSEC(1));
    assert_int_equal(loop->timer.count, 1);

    loop->timer.ptr[0].time = EVIO_TIME_MAX - 500;

    loop->time = EVIO_TIME_MAX - 500;

    evio_timer_update(loop);

    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

#define MANY_TIMERS 200
TEST(test_evio_timer_many)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm[MANY_TIMERS];
    for (size_t i = 0; i < MANY_TIMERS; ++i) {
        evio_timer_init(&tm[i], generic_cb, 0);
        tm[i].data = &data;
        evio_timer_start(loop, &tm[i], EVIO_TIME_FROM_MSEC(i + 1));
    }

    assert_int_equal(evio_refcount(loop), MANY_TIMERS);

    while (evio_refcount(loop) > 0) {
        evio_run(loop, EVIO_RUN_ONCE);
    }

    assert_int_equal(data.called, MANY_TIMERS);
    evio_loop_free(loop);
}

#define MANY_RANDOM_TIMERS 100
TEST(test_evio_timer_random)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm[MANY_RANDOM_TIMERS];
    unsigned int seed = 1;

    for (size_t i = 0; i < MANY_RANDOM_TIMERS; ++i) {
        evio_timer_init(&tm[i], generic_cb, 0);
        tm[i].data = &data;
        evio_timer_start(loop, &tm[i], EVIO_TIME_FROM_MSEC(rand_r(&seed) % 100));
    }
    assert_int_equal(evio_refcount(loop), MANY_RANDOM_TIMERS);

    while (evio_refcount(loop) > 0) {
        evio_run(loop, EVIO_RUN_ONCE);
    }

    assert_int_equal(data.called, MANY_RANDOM_TIMERS);
    evio_loop_free(loop);
}

TEST(test_evio_timer_fast_repeat)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, EVIO_TIME_FROM_MSEC(1));
    tm.data = &data;
    evio_timer_start(loop, &tm, 0);

    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(data.called, 1);

    assert_int_equal(evio_refcount(loop), 1);

    evio_timer_stop(loop, &tm);
    evio_loop_free(loop);
}

static void timer_slow_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    // Sleep for longer than the repeat interval
    usleep(20000); // 20ms
    generic_cb_data *data = base->data;
    data->called++;
}

TEST(test_evio_timer_slow_callback)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    // A timer that repeats every millisecond.
    evio_timer_init(&tm, timer_slow_cb, EVIO_TIME_FROM_MSEC(1));
    tm.data = &data;
    evio_timer_start(loop, &tm, 0);

    // The first callback will be called. It will sleep for 20ms.
    // The timer's repeat is 1ms. When the loop reschedules it, the new
    // expiration time will be in the past relative to when the callback
    // finishes, but not necessarily when it's rescheduled.
    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(data.called, 1);

    // It was rescheduled.
    assert_int_equal(evio_refcount(loop), 1);
    assert_true(tm.active);

    // The next expiry is <= repeat interval from the current time.
    // This can be flaky on a slow system, but <= is safe.
    assert_true(evio_timer_remaining(loop, &tm) <= tm.repeat);

    evio_timer_stop(loop, &tm);
    evio_loop_free(loop);
}

TEST(test_evio_timer_stop_middle)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm1;
    evio_timer_init(&tm1, generic_cb, 0);
    tm1.data = &data;
    evio_timer_start(loop, &tm1, 100);

    evio_timer tm2;
    evio_timer_init(&tm2, generic_cb, 0);
    tm2.data = &data;
    evio_timer_start(loop, &tm2, 200);

    evio_timer tm3;
    evio_timer_init(&tm3, generic_cb, 0);
    tm3.data = &data;
    evio_timer_start(loop, &tm3, 300);

    assert_int_equal(loop->timer.count, 3);
    assert_int_equal(evio_refcount(loop), 3);

    // Middle removal.
    evio_timer_stop(loop, &tm1);

    assert_int_equal(loop->timer.count, 2);
    assert_int_equal(evio_refcount(loop), 2);

    // Stop the rest (order doesn't matter)
    evio_timer_stop(loop, &tm2);
    evio_timer_stop(loop, &tm3); // double stop is safe

    assert_int_equal(loop->timer.count, 0);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_overflow)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    tm.data = &data;
    // This triggers the overflow check in evio_timer_start
    evio_timer_start(loop, &tm, EVIO_TIME_MAX);

    // Timer was not started.
    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_again_inactive_repeat)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, EVIO_TIME_FROM_SEC(1)); // has repeat, but not active
    tm.data = &data;

    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    // Call 'again' on an inactive timer that has a repeat value.
    // Start inactive timer with repeat.
    evio_timer_again(loop, &tm);

    assert_true(tm.active);
    assert_int_equal(evio_refcount(loop), 1);
    assert_true(evio_timer_remaining(loop, &tm) > 0);

    evio_timer_stop(loop, &tm);
    evio_loop_free(loop);
}

TEST(test_evio_timer_again_inactive_norepeat)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0); // one-shot, inactive
    tm.data = &data;

    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    // Call 'again' on an inactive, non-repeating timer.
    // No-op.
    evio_timer_again(loop, &tm);

    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_remaining_expired)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    tm.data = &data;
    evio_timer_start(loop, &tm, 0); // Start with 0 timeout

    // Before the loop runs, the timer's expiry time is loop->time + 0,
    // which is <= loop->time. So remaining is 0.
    assert_int_equal(evio_timer_remaining(loop, &tm), 0);

    // Now run the loop to confirm it fires.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    assert_false(tm.active); // one-shot timer is now inactive

    evio_loop_free(loop);
}

TEST(test_evio_timer_again_boundary)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    // Set repeat to a specific value
    evio_timer_init(&tm, generic_cb, 100);
    tm.data = &data;
    evio_timer_start(loop, &tm, 0);

    // Set loop->time exactly at EVIO_TIME_MAX - repeat (boundary condition)
    // Boundary: reschedule to EVIO_TIME_MAX.
    loop->time = EVIO_TIME_MAX - 100;

    // Fire the timer and reschedule
    evio_timer_again(loop, &tm);

    assert_true(tm.active);

    evio_timer_stop(loop, &tm);
    evio_loop_free(loop);
}

TEST(test_evio_timer_update_boundary)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 100);
    tm.data = &data;
    evio_timer_start(loop, &tm, 1000);

    assert_int_equal(loop->timer.count, 1);

    loop->timer.ptr[0].time = EVIO_TIME_MAX - 100;
    loop->time = EVIO_TIME_MAX - 100;

    evio_timer_update(loop);

    assert_true(tm.active);
    assert_int_equal(loop->timer.ptr[0].time, EVIO_TIME_MAX);

    evio_timer_stop(loop, &tm);
    evio_loop_free(loop);
}

TEST(test_evio_timer_catchup_overflow)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 1);
    tm.data = &data;
    evio_timer_start(loop, &tm, 1000);

    assert_int_equal(loop->timer.count, 1);

    loop->time = EVIO_TIME_MAX;
    loop->timer.ptr[0].time = EVIO_TIME_MAX;

    evio_timer_update(loop);

    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_catchup_time_max)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 1);
    tm.data = &data;
    evio_timer_start(loop, &tm, 1000);

    assert_int_equal(loop->timer.count, 1);

    loop->timer.ptr[0].time = EVIO_TIME_MAX - 1;
    loop->time = EVIO_TIME_MAX;

    evio_timer_update(loop);

    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_again_boundary_overflow)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 100);
    tm.data = &data;
    evio_timer_start(loop, &tm, 0);

    loop->time = EVIO_TIME_MAX - 100 + 1;

    evio_timer_again(loop, &tm);

    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_update_boundary_overflow)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 100);
    tm.data = &data;
    evio_timer_start(loop, &tm, 1000);

    assert_int_equal(loop->timer.count, 1);

    loop->timer.ptr[0].time = EVIO_TIME_MAX - 100 + 1;
    loop->time = EVIO_TIME_MAX - 100 + 1;

    evio_timer_update(loop);

    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

static void restart_zero_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    generic_cb(loop, base, emask);
    generic_cb_data *data = base->data;
    if (data->called < 3) {
        evio_timer_start(loop, container_of(base, evio_timer, base), 0);
    }
}

TEST(test_evio_timer_restart_zero)
{
    const int flags[] = { EVIO_RUN_ONCE, EVIO_RUN_NOWAIT };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
        assert_non_null(loop);
        generic_cb_data data = { 0 };
        evio_timer tm;
        evio_timer_init(&tm, restart_zero_cb, 0);
        tm.data = &data;
        evio_timer_start(loop, &tm, 0);

        for (size_t called = 1; called <= 3; ++called) {
            evio_run(loop, flags[i]);
            assert_int_equal(data.called, called);
            assert_int_equal(data.emask, EVIO_TIMER);
            assert_int_equal(evio_pending_count(loop), 0);
            assert_int_equal(evio_refcount(loop), called < 3);
        }

        evio_loop_free(loop);
    }
}

typedef struct {
    evio_timer *timers[3];
    size_t called;
} stop_pending_data;

static void stop_pending_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    stop_pending_data *data = base->data;
    evio_timer *self = container_of(base, evio_timer, base);
    assert_int_equal(emask, EVIO_TIMER);
    if (++data->called == 1) {
        evio_timer *other = self == data->timers[0] ? data->timers[1] : data->timers[0];
        assert_true(other->pending);
        evio_timer_stop(loop, other);
        evio_free(other);
    }
    evio_timer_stop(loop, self);
    evio_free(self);
}

TEST(test_evio_timer_stop_and_free_pending)
{
    for (evio_time repeat = 0; repeat < 2; ++repeat) {
        evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
        assert_non_null(loop);
        loop->time = 0;
        stop_pending_data data = { 0 };
        for (size_t i = 0; i < 3; ++i) {
            data.timers[i] = evio_calloc(1, sizeof(*data.timers[i]));
            evio_timer_init(data.timers[i], stop_pending_cb, repeat);
            data.timers[i]->data = &data;
            evio_timer_start(loop, data.timers[i], 0);
        }

        evio_timer_update(loop);
        assert_int_equal(data.called, 0);
        assert_int_equal(evio_pending_count(loop), 3);
        assert_int_equal(evio_refcount(loop), repeat ? 3 : 0);

        evio_invoke_pending(loop);
        assert_int_equal(data.called, 2);
        assert_int_equal(evio_pending_count(loop), 0);
        assert_int_equal(loop->timer.count, 0);
        assert_int_equal(evio_refcount(loop), 0);

        evio_timer_update(loop);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 2);
        evio_loop_free(loop);
    }
}

typedef struct {
    evio_timer timers[2];
    evio_timer *target;
    size_t called;
} pending_timer_data;

static void restart_pending_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    pending_timer_data *data = base->data;
    evio_timer *self = container_of(base, evio_timer, base);
    assert_int_equal(emask, EVIO_TIMER);
    evio_timer_stop(loop, self);
    if (++data->called == 1) {
        data->target = self == &data->timers[0] ? &data->timers[1] : &data->timers[0];
        assert_true(data->target->pending);
        evio_timer_stop(loop, data->target);
        evio_timer_start(loop, data->target, 0);
    }
}

TEST(test_evio_timer_restart_pending)
{
    for (evio_time repeat = 0; repeat < 2; ++repeat) {
        evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
        assert_non_null(loop);
        loop->time = 0;
        pending_timer_data data = { 0 };
        for (size_t i = 0; i < 2; ++i) {
            evio_timer_init(&data.timers[i], restart_pending_cb, repeat);
            data.timers[i].data = &data;
            evio_timer_start(loop, &data.timers[i], 0);
        }

        evio_timer_update(loop);
        assert_int_equal(data.called, 0);
        assert_int_equal(evio_pending_count(loop), 2);
        assert_int_equal(evio_refcount(loop), repeat ? 2 : 0);

        evio_invoke_pending(loop);
        assert_int_equal(data.called, 1);
        assert_non_null(data.target);
        assert_true(data.target->active);
        assert_false(data.target->pending);
        assert_int_equal(evio_timer_remaining(loop, data.target), 0);
        assert_int_equal(evio_pending_count(loop), 0);
        assert_int_equal(loop->timer.count, 1);
        assert_int_equal(evio_refcount(loop), 1);

        evio_timer_update(loop);
        assert_int_equal(data.called, 1);
        assert_int_equal(evio_pending_count(loop), 1);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 2);
        assert_false(data.target->active);
        assert_false(data.target->pending);
        assert_int_equal(evio_pending_count(loop), 0);
        assert_int_equal(loop->timer.count, 0);
        assert_int_equal(evio_refcount(loop), 0);

        evio_timer_update(loop);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 2);
        evio_loop_free(loop);
    }
}

static void again_pending_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    pending_timer_data *data = base->data;
    evio_timer *self = container_of(base, evio_timer, base);
    assert_int_equal(emask, EVIO_TIMER);
    evio_timer_stop(loop, self);
    if (++data->called == 1) {
        data->target = self == &data->timers[0] ? &data->timers[1] : &data->timers[0];
        assert_true(data->target->pending);
        evio_timer_again(loop, data->target);
    }
}

TEST(test_evio_timer_again_pending)
{
    const struct {
        evio_time repeat;
        evio_time next_repeat;
    } cases[] = {
        { .repeat = 0, .next_repeat = 0 },
        { .repeat = 0, .next_repeat = 10 },
        { .repeat = 1, .next_repeat = 0 },
        { .repeat = 1, .next_repeat = 10 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
        assert_non_null(loop);
        loop->time = 0;
        pending_timer_data data = { 0 };
        for (size_t j = 0; j < 2; ++j) {
            evio_timer_init(&data.timers[j], again_pending_cb, cases[i].repeat);
            data.timers[j].data = &data;
            evio_timer_start(loop, &data.timers[j], 0);
        }

        evio_timer_update(loop);
        assert_int_equal(data.called, 0);
        assert_int_equal(evio_pending_count(loop), 2);
        assert_int_equal(evio_refcount(loop), cases[i].repeat ? 2 : 0);
        for (size_t j = 0; j < 2; ++j) {
            evio_timer_set(&data.timers[j], cases[i].next_repeat);
        }

        evio_invoke_pending(loop);
        assert_int_equal(data.called, 1);
        assert_non_null(data.target);
        assert_false(data.target->pending);
        assert_int_equal(evio_timer_remaining(loop, data.target), cases[i].next_repeat);
        assert_int_equal(evio_pending_count(loop), 0);
        assert_int_equal(loop->timer.count, cases[i].next_repeat != 0);
        assert_int_equal(evio_refcount(loop), cases[i].next_repeat != 0);

        if (!cases[i].next_repeat) {
            assert_false(data.target->active);
            evio_loop_free(loop);
            continue;
        }

        assert_true(data.target->active);
        loop->time = cases[i].next_repeat - 1;
        evio_timer_update(loop);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 1);
        assert_int_equal(evio_timer_remaining(loop, data.target), 1);
        assert_int_equal(evio_pending_count(loop), 0);

        ++loop->time;
        evio_timer_update(loop);
        assert_int_equal(data.called, 1);
        assert_int_equal(evio_pending_count(loop), 1);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 2);
        assert_false(data.target->active);
        assert_false(data.target->pending);
        assert_int_equal(evio_pending_count(loop), 0);
        assert_int_equal(loop->timer.count, 0);
        assert_int_equal(evio_refcount(loop), 0);
        evio_loop_free(loop);
    }
}

TEST(test_evio_timer_repeat_from_loop_time)
{
    const evio_time times[] = { 10, 12, 35 };
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); ++i) {
        evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
        assert_non_null(loop);
        loop->time = 0;
        generic_cb_data data = { 0 };
        evio_timer tm;
        evio_timer_init(&tm, generic_cb, 10);
        tm.data = &data;
        evio_timer_start(loop, &tm, 10);

        loop->time = times[i];
        evio_timer_update(loop);
        assert_int_equal(data.called, 0);
        assert_int_equal(evio_pending_count(loop), 1);
        assert_int_equal(evio_timer_remaining(loop, &tm), 10);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 1);
        assert_int_equal(data.emask, EVIO_TIMER);
        assert_int_equal(evio_refcount(loop), 1);

        loop->time += 9;
        evio_timer_update(loop);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 1);
        assert_int_equal(evio_timer_remaining(loop, &tm), 1);

        ++loop->time;
        evio_timer_update(loop);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 2);
        assert_int_equal(data.emask, EVIO_TIMER);
        assert_int_equal(evio_timer_remaining(loop, &tm), 10);
        evio_timer_stop(loop, &tm);
        evio_loop_free(loop);
    }
}

typedef struct {
    size_t called;
    evio_mask emask;
    evio_time repeat;
} change_repeat_data;

static void change_repeat_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    change_repeat_data *data = base->data;
    data->called++;
    data->emask = emask;
    evio_timer_set(container_of(base, evio_timer, base), data->repeat);
}

TEST(test_evio_timer_repeat_changed_in_callback)
{
    const evio_time repeats[] = { 0, 20 };
    for (size_t i = 0; i < sizeof(repeats) / sizeof(repeats[0]); ++i) {
        evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
        assert_non_null(loop);
        loop->time = 0;
        change_repeat_data data = { .repeat = repeats[i] };
        evio_timer tm;
        evio_timer_init(&tm, change_repeat_cb, 10);
        tm.data = &data;
        evio_timer_start(loop, &tm, 10);

        loop->time = 12;
        evio_timer_update(loop);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 1);
        assert_int_equal(data.emask, EVIO_TIMER);
        assert_true(tm.active);
        assert_int_equal(evio_timer_remaining(loop, &tm), 10);

        loop->time = 21;
        evio_timer_update(loop);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 1);
        assert_int_equal(evio_timer_remaining(loop, &tm), 1);

        loop->time = 22;
        evio_timer_update(loop);
        assert_int_equal(data.called, 1);
        assert_int_equal(evio_pending_count(loop), 1);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 2);
        assert_int_equal(data.emask, EVIO_TIMER);
        assert_int_equal(evio_timer_remaining(loop, &tm), repeats[i]);
        assert_int_equal(tm.active != 0, repeats[i] != 0);
        assert_int_equal(evio_refcount(loop), repeats[i] != 0);
        evio_timer_stop(loop, &tm);
        evio_loop_free(loop);
    }
}

TEST(test_evio_timer_repeat_overflow_after_delay)
{
    const evio_time times[] = { EVIO_TIME_MAX - 9, EVIO_TIME_MAX };
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); ++i) {
        evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
        assert_non_null(loop);
        loop->time = 0;
        generic_cb_data data = { 0 };
        evio_timer tm;
        evio_timer_init(&tm, generic_cb, 10);
        tm.data = &data;
        evio_timer_start(loop, &tm, 10);

        loop->time = times[i];
        evio_timer_update(loop);
        assert_int_equal(data.called, 0);
        assert_false(tm.active);
        assert_int_equal(evio_pending_count(loop), 1);
        assert_int_equal(loop->timer.count, 0);
        assert_int_equal(evio_refcount(loop), 0);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 1);
        assert_int_equal(data.emask, EVIO_TIMER);
        assert_int_equal(evio_pending_count(loop), 0);

        evio_timer_update(loop);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 1);
        evio_loop_free(loop);
    }
}

TEST(test_evio_timer_equal_deadlines)
{
    enum { TIMER_COUNT = 17 };
    const evio_time repeats[] = { 0, 10 };
    for (size_t i = 0; i < sizeof(repeats) / sizeof(repeats[0]); ++i) {
        evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
        assert_non_null(loop);
        loop->time = 0;
        evio_timer timers[TIMER_COUNT];
        generic_cb_data data[TIMER_COUNT] = { 0 };
        for (size_t j = 0; j < TIMER_COUNT; ++j) {
            evio_timer_init(&timers[j], generic_cb, repeats[i]);
            timers[j].data = &data[j];
            evio_timer_start(loop, &timers[j], 10);
        }

        for (size_t round = 1; round <= 2; ++round) {
            loop->time += 10;
            evio_timer_update(loop);
            assert_int_equal(evio_pending_count(loop),
                             repeats[i] || round == 1 ? TIMER_COUNT : 0);
            evio_invoke_pending(loop);
            assert_int_equal(evio_pending_count(loop), 0);
            assert_int_equal(evio_refcount(loop), repeats[i] ? TIMER_COUNT : 0);
            for (size_t j = 0; j < TIMER_COUNT; ++j) {
                assert_int_equal(data[j].called, repeats[i] ? round : 1);
                assert_int_equal(data[j].emask, EVIO_TIMER);
                assert_false(timers[j].pending);
                assert_int_equal(timers[j].active != 0, repeats[i] != 0);
            }
        }

        for (size_t j = 0; j < TIMER_COUNT; ++j) {
            evio_timer_stop(loop, &timers[j]);
        }
        assert_int_equal(loop->timer.count, 0);
        assert_int_equal(evio_refcount(loop), 0);
        evio_loop_free(loop);
    }
}

TEST(test_evio_timer_again_pending_overflow)
{
    const evio_time repeats[] = { 0, 10 };
    for (size_t i = 0; i < sizeof(repeats) / sizeof(repeats[0]); ++i) {
        evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
        assert_non_null(loop);
        loop->time = 0;
        generic_cb_data data = { 0 };
        evio_timer timer;
        evio_timer_init(&timer, generic_cb, repeats[i]);
        timer.data = &data;
        evio_timer_start(loop, &timer, 0);
        evio_timer_update(loop);
        assert_int_equal(evio_pending_count(loop), 1);
        assert_int_equal(timer.active != 0, repeats[i] != 0);
        assert_int_equal(data.called, 0);

        evio_timer_set(&timer, 10);
        loop->time = EVIO_TIME_MAX - 9;
        evio_timer_again(loop, &timer);
        assert_false(timer.active);
        assert_false(timer.pending);
        assert_int_equal(timer.repeat, 10);
        assert_int_equal(loop->timer.count, 0);
        assert_int_equal(evio_pending_count(loop), 0);
        assert_int_equal(evio_refcount(loop), 0);
        evio_timer_update(loop);
        evio_invoke_pending(loop);
        assert_int_equal(data.called, 0);
        evio_loop_free(loop);
    }
}

TEST(test_evio_timer_start_boundary)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    loop->time = EVIO_TIME_MAX - 10;
    generic_cb_data data = { 0 };
    evio_timer timer;
    evio_timer_init(&timer, generic_cb, 0);
    timer.data = &data;
    evio_timer_start(loop, &timer, 10);
    assert_true(timer.active);
    assert_int_equal(evio_timer_remaining(loop, &timer), 10);
    assert_int_equal(evio_refcount(loop), 1);

    loop->time = EVIO_TIME_MAX;
    evio_timer_update(loop);
    assert_int_equal(evio_pending_count(loop), 1);
    evio_invoke_pending(loop);
    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_TIMER);
    assert_false(timer.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_timer_start(loop, &timer, 1);
    assert_false(timer.active);
    assert_int_equal(loop->timer.count, 0);
    assert_int_equal(evio_refcount(loop), 0);
    evio_timer_update(loop);
    evio_invoke_pending(loop);
    assert_int_equal(evio_pending_count(loop), 0);
    assert_int_equal(data.called, 1);
    evio_loop_free(loop);
}

TEST(test_evio_timer_start_overflow)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);

    evio_timer_start(loop, &tm, EVIO_TIME_MAX);
    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}
