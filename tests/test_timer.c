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
    evio_poll *io = (evio_poll *)base;
    read(io->fd, buf, sizeof(buf));
    generic_cb(loop, base, emask);
}

TEST(test_evio_large_timeout)
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

    loop->timer.ptr[0].time = evio_get_time(loop) +
                              (EVIO_TIME_C(INT_MAX) + 1) * EVIO_TIME_PER_MSEC;

    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_ONCE); // This will call evio_timeout

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

    // Double start should be a no-op
    evio_timer_start(loop, &tm, 0);
    assert_int_equal(evio_refcount(loop), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_TIMER);

    // Timer should be stopped now (one-shot)
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

    // Timer is repeating, so it's rescheduled for 10s from now.
    // The remaining time should be close to 10s.
    assert_true(evio_timer_remaining(loop, &tm) <= EVIO_TIME_FROM_SEC(10));

    // Restart it with 'again'
    evio_timer_again(loop, &tm);
    assert_int_equal(data.called, 0);

    // It should be rescheduled for another 10s from the *new* current time
    assert_true(evio_timer_remaining(loop, &tm) <= EVIO_TIME_FROM_SEC(10));

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0); // should not have fired yet

    evio_timer_stop(loop, &tm);
    assert_int_equal(evio_timer_remaining(loop, &tm), 0);
    assert_int_equal(evio_refcount(loop), 0);

    // Test again on a stopped timer with a repeat value
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

    // The loop should time out.
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

    // 'again' on a non-repeating timer should stop it.
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

    // This should stop the timer due to overflow check in evio_timer_again.
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
    assert_int_equal(evio_refcount(loop), 0); // should be stopped
    assert_false(tm.active);

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
        // Use small, slightly varied timeouts to stress heap
        evio_timer_init(&tm[i], generic_cb, 0);
        tm[i].data = &data;
        evio_timer_start(loop, &tm[i], EVIO_TIME_FROM_MSEC(i + 1));
    }

    assert_int_equal(evio_refcount(loop), MANY_TIMERS);

    // Run until all timers have fired
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
    srand(time(NULL));

    for (size_t i = 0; i < MANY_RANDOM_TIMERS; ++i) {
        evio_timer_init(&tm[i], generic_cb, 0);
        tm[i].data = &data;
        // random timeout between 0 and 99 ms
        evio_timer_start(loop, &tm[i], EVIO_TIME_FROM_MSEC(rand() % 100));
    }
    assert_int_equal(evio_refcount(loop), MANY_RANDOM_TIMERS);

    // Run until all timers have fired
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
    // A timer that repeats every millisecond.
    evio_timer_init(&tm, generic_cb, EVIO_TIME_FROM_MSEC(1));
    tm.data = &data;
    evio_timer_start(loop, &tm, 0);

    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(data.called, 1);

    // It should have been rescheduled.
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

    // It should have been rescheduled.
    assert_int_equal(evio_refcount(loop), 1);
    assert_true(tm.active);

    // The next expiry should be <= repeat interval from the current time.
    // This can be flaky on a slow system, but <= should be safe.
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

    // Stop a timer that is not the last one in the list.
    // This will trigger the heap adjustment logic in evio_timer_stop.
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
    // This should trigger the overflow check in evio_timer_start
    evio_timer_start(loop, &tm, EVIO_TIME_MAX);

    // The timer should not have been started.
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
    // It should start the timer.
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
    // This should be a no-op.
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
    // which is <= loop->time. So remaining should be 0.
    assert_int_equal(evio_timer_remaining(loop, &tm), 0);

    // Now run the loop to confirm it fires.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    assert_false(tm.active); // one-shot timer is now inactive

    evio_loop_free(loop);
}
