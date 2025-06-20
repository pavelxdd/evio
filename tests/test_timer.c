#include "test.h"

TEST(test_evio_timeout_huge_ns_diff)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    evio_timer_start(loop, &tm, 1); // Start with a dummy timeout

    evio_poll io;
    evio_poll_init(&io, read_and_count_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Manually set timer expiration to the maximum to create a huge nanosecond
    // difference. Note this test may be flaky if system uptime is very small.
    loop->timer.ptr[0].time = EVIO_TIME_MAX;

    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_ONCE); // This will call evio_timeout

    // Poll watcher fired, timer did not.
    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb_emask, 0); // read_and_count_cb doesn't set this

    // Cleanup
    evio_timer_stop(loop, &tm);
    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_timeout_huge_ms_diff)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    evio_timer_start(loop, &tm, 1); // Start with a dummy timeout

    evio_poll io;
    evio_poll_init(&io, read_and_count_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Manually set timer expiration far enough in the future to overflow a
    // signed int when converted to milliseconds, but not far enough to trigger
    // the initial nanosecond overflow check.
    loop->timer.ptr[0].time = evio_get_time(loop) +
                              (EVIO_TIME_C(INT_MAX) + 1) * EVIO_TIME_PER_MSEC;

    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_ONCE); // This will call evio_timeout

    // Poll watcher fired, timer did not.
    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb_emask, 0);

    // Cleanup
    evio_timer_stop(loop, &tm);
    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_timer)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    evio_timer_start(loop, &tm, 0); // Start with 0 timeout to fire immediately

    assert_int_equal(evio_refcount(loop), 1);
    assert_int_equal(generic_cb_called, 0);

    // Double start should be a no-op
    evio_timer_start(loop, &tm, 0);
    assert_int_equal(evio_refcount(loop), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb_emask, EVIO_TIMER);

    // Timer should be stopped now (one-shot)
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_repeat)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 1);
    evio_timer_start(loop, &tm, 0);

    assert_int_equal(evio_refcount(loop), 1);

    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(evio_refcount(loop), 1); // Should still be active

    usleep(20000); // 20ms

    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(generic_cb_called, 2);

    evio_timer_stop(loop, &tm);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

static size_t run_until_called_or_timeout(evio_loop *loop, size_t loop_limit, int flags)
{
    size_t loops = 0;
    while (generic_cb_called == 0 && loops < loop_limit) {
        evio_run(loop, flags);
        loops++;
    }
    return loops;
}

TEST(test_evio_timer_again_and_remaining)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    // repeating timer, 10 seconds
    evio_timer_init(&tm, generic_cb, EVIO_TIME_FROM_SEC(10));
    // Start with a timeout larger than the coarse clock resolution to make the test stable.
    evio_timer_start(loop, &tm, EVIO_TIME_FROM_MSEC(50));

    assert_true(evio_timer_remaining(loop, &tm) > 0);

    // On a busy CI server, the single run might not be enough due to clock granularity.
    // We loop until the timer fires, with a safety break.
    run_until_called_or_timeout(loop, 10, EVIO_RUN_ONCE);

    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb_emask, EVIO_TIMER);
    reset_cb_state();

    // Timer is repeating, so it's rescheduled for 10s from now.
    // The remaining time should be close to 10s.
    assert_true(evio_timer_remaining(loop, &tm) <= EVIO_TIME_FROM_SEC(10));

    // Restart it with 'again'
    evio_timer_again(loop, &tm);
    assert_int_equal(generic_cb_called, 0);

    // It should be rescheduled for another 10s from the *new* current time
    assert_true(evio_timer_remaining(loop, &tm) <= EVIO_TIME_FROM_SEC(10));

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0); // should not have fired yet

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
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    // Start with a long timeout that won't fire.
    evio_timer_start(loop, &tm, EVIO_TIME_FROM_SEC(10));

    // The loop should time out.
    size_t loops = run_until_called_or_timeout(loop, 5, EVIO_RUN_NOWAIT);
    assert_int_equal(loops, 5);
    assert_int_equal(generic_cb_called, 0);

    evio_timer_stop(loop, &tm);
    evio_loop_free(loop);
}

TEST(test_evio_timer_again_one_shot)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0); // one-shot
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
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, EVIO_TIME_MAX); // large repeat
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
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, EVIO_TIME_MAX); // large repeat
    evio_timer_start(loop, &tm, 0);                  // fire immediately

    assert_int_equal(evio_refcount(loop), 1);

    // This run will fire the timer. When rescheduling in evio_timer_update,
    // it will hit the overflow check and stop the timer instead of repeating.
    evio_run(loop, EVIO_RUN_ONCE);

    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(evio_refcount(loop), 0); // should be stopped
    assert_false(tm.active);

    evio_loop_free(loop);
}

#define MANY_TIMERS 200
TEST(test_evio_timer_many)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm[MANY_TIMERS];
    for (size_t i = 0; i < MANY_TIMERS; ++i) {
        // Use small, slightly varied timeouts to stress heap
        evio_timer_init(&tm[i], generic_cb, 0);
        evio_timer_start(loop, &tm[i], EVIO_TIME_FROM_MSEC(i + 1));
    }

    assert_int_equal(evio_refcount(loop), MANY_TIMERS);

    // Run until all timers have fired
    while (evio_refcount(loop) > 0) {
        evio_run(loop, EVIO_RUN_ONCE);
    }

    assert_int_equal(generic_cb_called, MANY_TIMERS);
    evio_loop_free(loop);
}

#define MANY_RANDOM_TIMERS 100
TEST(test_evio_timer_random)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm[MANY_RANDOM_TIMERS];
    srand(time(NULL));

    for (size_t i = 0; i < MANY_RANDOM_TIMERS; ++i) {
        evio_timer_init(&tm[i], generic_cb, 0);
        // random timeout between 0 and 99 ms
        evio_timer_start(loop, &tm[i], EVIO_TIME_FROM_MSEC(rand() % 100));
    }
    assert_int_equal(evio_refcount(loop), MANY_RANDOM_TIMERS);

    // Run until all timers have fired
    while (evio_refcount(loop) > 0) {
        evio_run(loop, EVIO_RUN_ONCE);
    }

    assert_int_equal(generic_cb_called, MANY_RANDOM_TIMERS);
    evio_loop_free(loop);
}

TEST(test_evio_timer_large_timeout)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    // This will test the large timeout logic in evio_timeout
    evio_timer_start(loop, &tm, EVIO_TIME_MAX / 2);

    // Run with nowait, it shouldn't block, but calculate a large timeout.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // The timer shouldn't have fired.
    assert_int_equal(generic_cb_called, 0);

    evio_timer_stop(loop, &tm);
    evio_loop_free(loop);
}

TEST(test_evio_timer_fast_repeat)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    // A timer that repeats every millisecond.
    evio_timer_init(&tm, generic_cb, EVIO_TIME_FROM_MSEC(1));
    evio_timer_start(loop, &tm, 0);

    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(generic_cb_called, 1);

    // It should have been rescheduled.
    assert_int_equal(evio_refcount(loop), 1);

    evio_timer_stop(loop, &tm);
    evio_loop_free(loop);
}

static void timer_slow_cb(evio_loop *loop, evio_base *w, evio_mask emask)
{
    // Sleep for longer than the repeat interval
    usleep(20000); // 20ms
    generic_cb_called++;
}

TEST(test_evio_timer_slow_callback)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    // A timer that repeats every millisecond.
    evio_timer_init(&tm, timer_slow_cb, EVIO_TIME_FROM_MSEC(1));
    evio_timer_start(loop, &tm, 0);

    // The first callback will be called. It will sleep for 20ms.
    // The timer's repeat is 1ms. When the loop reschedules it, the new
    // expiration time will be in the past relative to when the callback
    // finishes, but not necessarily when it's rescheduled.
    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(generic_cb_called, 1);

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
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm1, tm2, tm3;
    evio_timer_init(&tm1, generic_cb, 0);
    evio_timer_init(&tm2, generic_cb, 0);
    evio_timer_init(&tm3, generic_cb, 0);

    // Start timers with different timeouts to control heap order.
    evio_timer_start(loop, &tm1, 100);
    evio_timer_start(loop, &tm2, 200);
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
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    // This should trigger the overflow check in evio_timer_start
    evio_timer_start(loop, &tm, EVIO_TIME_MAX);

    // The timer should not have been started.
    assert_false(tm.active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_again_inactive_repeat)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, EVIO_TIME_FROM_SEC(1)); // has repeat, but not active

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
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0); // one-shot, inactive

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
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, generic_cb, 0);
    evio_timer_start(loop, &tm, 0); // Start with 0 timeout

    // Before the loop runs, the timer's expiry time is loop->time + 0,
    // which is <= loop->time. So remaining should be 0.
    assert_int_equal(evio_timer_remaining(loop, &tm), 0);

    // Now run the loop to confirm it fires.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
    assert_false(tm.active); // one-shot timer is now inactive

    evio_loop_free(loop);
}
