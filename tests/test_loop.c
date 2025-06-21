#include "test.h"

#include <sys/resource.h>

static jmp_buf abort_jmp_buf;
static size_t custom_abort_called;

static FILE *custom_abort_handler(void *ctx)
{
    custom_abort_called++;
    longjmp(abort_jmp_buf, 1);
    return NULL; // GCOVR_EXCL_LINE
}

static FILE *leaking_loop_test_abort_handler(void *ctx)
{
    evio_loop_free(ctx);
    return custom_abort_handler(NULL);
}

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

static void break_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *counter = base->data;
    (*counter)++;
    evio_break(loop, EVIO_BREAK_ALL);
}

// GCOVR_EXCL_START
static void dummy_cb(evio_loop *loop, evio_base *base, evio_mask emask) {}
// GCOVR_EXCL_STOP

TEST(test_evio_clock_gettime_fail)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Use a test-only abort handler that can free the loop before jumping
    evio_set_abort(leaking_loop_test_abort_handler, loop);
    custom_abort_called = 0;

    // Set an invalid clock_id to force clock_gettime() to fail.
    loop->clock_id = -1;

    if (setjmp(abort_jmp_buf) == 0) {
        // Calling evio_update_time will call the faulty evio_clock_gettime
        evio_update_time(loop);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(custom_abort_called, 1);

    // Restore original abort handler. The loop is freed by the test handler.
    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_loop_new_fail)
{
    struct rlimit old_lim;
    // GCOVR_EXCL_START
    if (getrlimit(RLIMIT_NOFILE, &old_lim) != 0) {
        print_message("      -> Skipping test, could not get rlimit\n");
        return;
    }
    // GCOVR_EXCL_STOP

    // Find the number of open fds and set the limit to that value.
    // This will cause the next fd allocation to fail.
    int next_fd = dup(0);
    close(next_fd);

    struct rlimit new_lim;
    new_lim.rlim_cur = next_fd;
    new_lim.rlim_max = old_lim.rlim_max;

    // GCOVR_EXCL_START
    if (setrlimit(RLIMIT_NOFILE, &new_lim) != 0) {
        print_message("      -> Skipping test, could not set rlimit\n");
        setrlimit(RLIMIT_NOFILE, &old_lim);
        return;
    }
    // GCOVR_EXCL_STOP

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_null(loop);

    setrlimit(RLIMIT_NOFILE, &old_lim);
}

TEST(test_evio_userdata)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    assert_null(evio_get_userdata(loop));

    int x = 42;
    evio_set_userdata(loop, &x);
    assert_ptr_equal(evio_get_userdata(loop), &x);
    assert_int_equal(*(int *)evio_get_userdata(loop), 42);

    evio_set_userdata(loop, NULL);
    assert_null(evio_get_userdata(loop));

    evio_loop_free(loop);
}

TEST(test_evio_clockid)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    for (int i = 0; i < 2; ++i) {
        // Get default clock and check it's one of the expected values
        clockid_t old_clock = evio_get_clockid(loop);
        // GCOVR_EXCL_START
        assert_true(old_clock == CLOCK_MONOTONIC ||
                    old_clock == CLOCK_MONOTONIC_COARSE);
        // GCOVR_EXCL_STOP

        // Determine the alternative clock to switch to
        clockid_t new_clock = (old_clock == CLOCK_MONOTONIC)
                              ? CLOCK_MONOTONIC_COARSE
                              : CLOCK_MONOTONIC;

        struct timespec ts;
        // GCOVR_EXCL_START
        if (clock_getres(new_clock, &ts) != 0) {
            print_message(" -> Skipping clock switch test, clock_id %d not available\n", new_clock);
            continue;
        }
        // GCOVR_EXCL_STOP

        evio_set_clockid(loop, new_clock);
        assert_int_equal(evio_get_clockid(loop), new_clock);

        // Also check that time updates with the new clock
        evio_time time1 = evio_get_time(loop);
        // Sleep for a short duration to ensure the clock ticks.
        usleep(20000); // 20ms
        evio_update_time(loop);
        evio_time time2 = evio_get_time(loop);
        assert_true(time2 > time1);
    }

    evio_loop_free(loop);
}

TEST(test_evio_time_update)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_time time1 = evio_get_time(loop);
    assert_true(time1 > 0);

    // Sleep for a short duration to ensure the clock ticks.
    struct timespec req = { .tv_sec = 0, .tv_nsec = 2000000 }; // 2ms
    clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);

    evio_update_time(loop);
    evio_time time2 = evio_get_time(loop);
    assert_true(time2 > time1);

    // Running the loop also updates time.
    clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_time time3 = evio_get_time(loop);
    assert_true(time3 > time2);

    evio_loop_free(loop);
}

static void break_one_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *counter = base->data;
    ++(*counter);

    evio_break(loop, EVIO_BREAK_ONE);
}
static void break_all_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *counter = base->data;
    ++(*counter);

    evio_break(loop, EVIO_BREAK_ALL);
}

static evio_idle break_all_watcher;

static void nested_run_trigger_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *counter = base->data;
    ++(*counter);

    // Start the watcher that will break out of all loops. Using an idle watcher
    // ensures the nested loop doesn't block, as it forces a zero timeout.
    evio_idle_start(loop, &break_all_watcher);

    // This nested run will be broken by break_all_cb
    evio_run(loop, EVIO_RUN_DEFAULT);

    // The nested run has returned. The outer loop will break in its next check.
    assert_int_equal(evio_break_state(loop), EVIO_BREAK_ALL);
}

TEST(test_evio_break_one)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, break_one_cb);
    evio_prepare_start(loop, &prepare);

    size_t counter = 0;
    prepare.data = &counter;

    // evio_run should execute one iteration, then break.
    int active = evio_run(loop, EVIO_RUN_DEFAULT);
    assert_int_equal(counter, 1);
    assert_true(active);

    // Running again should do the same.
    active = evio_run(loop, EVIO_RUN_DEFAULT);
    assert_int_equal(counter, 2);
    assert_true(active);

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

TEST(test_evio_break_all)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, nested_run_trigger_cb, 0); // one-shot timer
    evio_timer_start(loop, &tm, 0); // Fire immediately

    size_t nested_run_trigger_cb_called = 0;
    tm.data = &nested_run_trigger_cb_called;

    evio_idle_init(&break_all_watcher, break_all_cb);
    size_t break_all_called = 0;
    break_all_watcher.data = &break_all_called;

    // This run will enter a nested loop, which will be broken by EVIO_BREAK_ALL,
    // which should propagate and break this outer loop too.
    int active = evio_run(loop, EVIO_RUN_DEFAULT);

    assert_int_equal(nested_run_trigger_cb_called, 1);
    assert_int_equal(break_all_called, 1);
    // The loop was forcibly stopped, so evio_run should return 0 (false).
    assert_false(active);

    // The break state is EVIO_BREAK_ALL upon exiting the run.
    assert_int_equal(evio_break_state(loop), EVIO_BREAK_ALL);

    // Stop the watcher that caused the break. Now refcount is 0.
    evio_idle_stop(loop, &break_all_watcher);
    assert_int_equal(evio_refcount(loop), 0);

    // Running again should exit immediately as there are no active watchers.
    active = evio_run(loop, EVIO_RUN_DEFAULT);
    assert_false(active);
    assert_int_equal(nested_run_trigger_cb_called, 1); // Should not have been called again
    assert_int_equal(break_all_called, 1);  // Should not have been called again

    evio_loop_free(loop);
}

TEST(test_evio_init_assert_null_cb)
{
    assert_non_null(state);
    assert_null(*state);

    volatile evio_cb cb = (evio_cb)(*state);

    evio_base base;
    expect_assert_failure(evio_init(&base, cb));
}

TEST(test_evio_invoke_assert_null_cb)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_base base = { 0 };
    expect_assert_failure(evio_invoke(loop, &base, EVIO_NONE));

    evio_loop_free(loop);
}

TEST(test_evio_invoke)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, generic_cb);
    prepare.data = &data;

    assert_int_equal(data.called, 0);
    evio_invoke(loop, &prepare.base, EVIO_PREPARE);
    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_PREPARE);

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

TEST(test_evio_ref_unref)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    assert_int_equal(evio_refcount(loop), 0);

    evio_ref(loop);
    assert_int_equal(evio_refcount(loop), 1);

    evio_ref(loop);
    assert_int_equal(evio_refcount(loop), 2);

    evio_unref(loop);
    assert_int_equal(evio_refcount(loop), 1);

    evio_unref(loop);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_ref_overflow_abort)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_set_abort(leaking_loop_test_abort_handler, loop);
    custom_abort_called = 0;

    // Manually set refcount to its maximum value to test overflow.
    loop->refcount = SIZE_MAX;

    if (setjmp(abort_jmp_buf) == 0) {
        evio_ref(loop); // This should overflow to 0 and trigger the abort.
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_loop_unref_abort)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    assert_int_equal(evio_refcount(loop), 0);

    evio_set_abort(leaking_loop_test_abort_handler, loop);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        evio_unref(loop); // Should abort
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_loop_free_all_lists)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Start one of each list-based watcher to ensure their lists are allocated
    evio_idle idle;
    evio_idle_init(&idle, dummy_cb);
    evio_idle_start(loop, &idle);

    evio_async async;
    evio_async_init(&async, dummy_cb);
    evio_async_start(loop, &async);

    evio_prepare prepare;
    evio_prepare_init(&prepare, dummy_cb);
    evio_prepare_start(loop, &prepare);

    evio_check check;
    evio_check_init(&check, dummy_cb);
    evio_check_start(loop, &check);

    evio_cleanup cleanup;
    evio_cleanup_init(&cleanup, dummy_cb);
    evio_cleanup_start(loop, &cleanup);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_once once;
    evio_once_init(&once, dummy_cb, fds[0], EVIO_READ);
    evio_once_start(loop, &once, 100);

    // Also need a plain poll watcher to allocate loop->fds
    evio_poll io;
    evio_poll_init(&io, dummy_cb, fds[1], EVIO_READ);
    evio_poll_start(loop, &io);

    // Freeing the loop should clean up all internal lists.
    // Valgrind will detect leaks if it doesn't.
    evio_loop_free(loop);

    close(fds[0]);
    close(fds[1]);
}

TEST(test_evio_run_break_all_set)
{
    size_t break_cb_called = 0;
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, break_cb);
    prepare.data = &break_cb_called;
    evio_prepare_start(loop, &prepare);

    evio_check check;
    evio_check_init(&check, generic_cb);
    check.data = &data;
    evio_check_start(loop, &check);

    // Loop should run the prepare callback, which sets break, and then exit
    // without running the check callback.
    evio_run(loop, EVIO_RUN_DEFAULT);

    assert_int_equal(break_cb_called, 1);
    assert_int_equal(data.called, 0);

    evio_prepare_stop(loop, &prepare);
    evio_check_stop(loop, &check);
    evio_loop_free(loop);
}

static void repeating_timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *counter = base->data;
    if (++(*counter) >= 3) {
        evio_timer *tm = (evio_timer *)base;
        evio_timer_stop(loop, tm);
    }
}

TEST(test_evio_run_default_looping)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_timer tm;
    evio_timer_init(&tm, repeating_timer_cb, 1); // repeat every 1ns
    evio_timer_start(loop, &tm, 0);

    size_t counter = 0;
    tm.data = &counter;

    // This will run until the refcount becomes 0
    int active = evio_run(loop, EVIO_RUN_DEFAULT);

    assert_int_equal(counter, 3);
    assert_false(active);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

static void pending_and_no_ref_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *counter = base->data;
    if (++(*counter) == 1) {
        // Stop myself, making refcount 0
        evio_prepare_stop(loop, (evio_prepare *)base);
        assert_int_equal(evio_refcount(loop), 0);

        // Queue an event for myself (I am inactive now).
        // Use internal queue_event to bypass the 'active' check and create
        // the test condition: refcount=0, pending>0.
        evio_queue_event(loop, base, EVIO_PREPARE);
        assert_int_equal(evio_pending_count(loop), 1);

        // Break the loop so we can check the return value of evio_run
        evio_break(loop, EVIO_BREAK_ONE);
    }
}

TEST(test_evio_run_return_pending)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, pending_and_no_ref_cb);
    evio_prepare_start(loop, &prepare);
    assert_int_equal(evio_refcount(loop), 1);

    size_t counter = 0;
    prepare.data = &counter;

    // evio_run will call the callback. The callback stops the watcher, queues
    // another event, and breaks. The draining invoke_pending will immediately
    // process the second event.
    int active = evio_run(loop, EVIO_RUN_DEFAULT);

    // After evio_run, the watcher is stopped (refcount=0) and the pending
    // queue is empty. So active should be false.
    assert_false(active);
    // The callback is called twice: once for the initial event, and a second
    // time for the event queued within the callback itself.
    assert_int_equal(counter, 2);
    assert_int_equal(evio_refcount(loop), 0);
    assert_int_equal(evio_pending_count(loop), 0);

    // Calling this again should do nothing as the pending queue is empty.
    evio_invoke_pending(loop);
    assert_int_equal(counter, 2);
    assert_int_equal(evio_pending_count(loop), 0);

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

static void break_and_pending_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *counter = base->data;
    // Only do this on the first call to avoid infinite loop in test
    if (++(*counter) == 1) {
        // Queue another event for myself. It will be processed in the next iteration.
        evio_feed_event(loop, base, EVIO_PREPARE);
        assert_int_equal(evio_pending_count(loop), 1);
        evio_break(loop, EVIO_BREAK_ONE);
    }
}

TEST(test_evio_run_return_ref_and_pending)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, break_and_pending_cb);
    evio_prepare_start(loop, &prepare);
    assert_int_equal(evio_refcount(loop), 1);

    size_t counter = 0;
    prepare.data = &counter;

    // The callback will be called once. It queues another event for itself
    // and then breaks. The draining invoke_pending will immediately process
    // the second event.
    int active = evio_run(loop, EVIO_RUN_DEFAULT);

    // The watcher is still active, so the return value should be true.
    assert_true(active);
    // The callback is called twice. The first call queues a second event, which
    // is processed immediately by the same evio_invoke_pending() call.
    assert_int_equal(counter, 2);
    assert_int_equal(evio_refcount(loop), 1);
    assert_int_equal(evio_pending_count(loop), 0);

    // Calling this again should do nothing.
    evio_invoke_pending(loop);
    assert_int_equal(counter, 2);

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

static void break_extra_flags_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *counter = base->data;
    (*counter)++;

    evio_break(loop, EVIO_BREAK_ONE | 0xff00);
}

TEST(test_evio_break_extra_flags)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, break_extra_flags_cb);
    evio_prepare_start(loop, &prepare);

    size_t counter = 0;
    prepare.data = &counter;

    int active = evio_run(loop, EVIO_RUN_DEFAULT);

    assert_int_equal(counter, 1);
    // evio_break() masks the state with (EVIO_BREAK_ONE | EVIO_BREAK_ALL),
    // so the internal state becomes EVIO_BREAK_ONE.
    // evio_run() then consumes this state and resets it to EVIO_BREAK_CANCEL
    // before returning. Thus, the state after the call is CANCEL.
    assert_int_equal(evio_break_state(loop), EVIO_BREAK_CANCEL);
    assert_true(active); // watcher is still active

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

static evio_prepare prepare_l1, prepare_l2, prepare_l3, prepare_l1_sibling;
static size_t level1_cb_called = 0;
static size_t level2_cb_called = 0;
static size_t level3_cb_called = 0;
static size_t level1_sibling_cb_called = 0;

// Level 1: Queues level 2 event and makes a nested call to invoke_pending.
static void nested_cb_level1(evio_loop *loop, evio_base *base, evio_mask emask)
{
    level1_cb_called++;
    evio_prepare_stop(loop, (evio_prepare *)base);
    evio_feed_event(loop, &prepare_l2.base, EVIO_PREPARE);
}

// Sibling to Level 1.
static void nested_cb_level1_sibling(evio_loop *loop, evio_base *base, evio_mask emask)
{
    level1_sibling_cb_called++;
    evio_prepare_stop(loop, (evio_prepare *)base);
}

// Level 2: Queues level 3 event.
static void nested_cb_level2(evio_loop *loop, evio_base *base, evio_mask emask)
{
    level2_cb_called++;
    evio_prepare_stop(loop, (evio_prepare *)base);
    evio_feed_event(loop, &prepare_l3.base, EVIO_PREPARE);
}

// Level 3: The innermost callback. It just marks that it was called.
static void nested_cb_level3(evio_loop *loop, evio_base *base, evio_mask emask)
{
    level3_cb_called++;
    evio_prepare_stop(loop, (evio_prepare *)base);
}

TEST(test_evio_nested_invoke_no_stealing)
{
    // Reset state
    level1_cb_called = 0;
    level2_cb_called = 0;
    level3_cb_called = 0;
    level1_sibling_cb_called = 0;

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Init all watchers
    evio_prepare_init(&prepare_l1, nested_cb_level1);
    evio_prepare_init(&prepare_l2, nested_cb_level2);
    evio_prepare_init(&prepare_l3, nested_cb_level3);
    evio_prepare_init(&prepare_l1_sibling, nested_cb_level1_sibling);

    evio_prepare_start(loop, &prepare_l1);
    evio_prepare_start(loop, &prepare_l2);
    evio_prepare_start(loop, &prepare_l3);
    evio_prepare_start(loop, &prepare_l1_sibling);

    // Queue two events in the initial buffer.
    evio_feed_event(loop, &prepare_l1_sibling.base, EVIO_PREPARE);
    evio_feed_event(loop, &prepare_l1.base, EVIO_PREPARE);

    // The single call to invoke_pending should drain the entire chain of events.
    evio_invoke_pending(loop);

    assert_int_equal(level1_cb_called, 1);
    assert_int_equal(level1_sibling_cb_called, 1);
    assert_int_equal(level2_cb_called, 1);
    assert_int_equal(level3_cb_called, 1);
    assert_int_equal(evio_pending_count(loop), 0);

    level1_cb_called = 0;
    level2_cb_called = 0;
    level3_cb_called = 0;
    level1_sibling_cb_called = 0;

    evio_prepare_start(loop, &prepare_l1);
    evio_prepare_start(loop, &prepare_l2);
    evio_prepare_start(loop, &prepare_l3);
    evio_prepare_start(loop, &prepare_l1_sibling);

    // The same test but with evio_run.
    evio_run(loop, EVIO_RUN_DEFAULT);

    assert_int_equal(level1_cb_called, 1);
    assert_int_equal(level1_sibling_cb_called, 1);
    assert_int_equal(level2_cb_called, 1);
    assert_int_equal(level3_cb_called, 1);
    assert_int_equal(evio_pending_count(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timeout_idle_coverage)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // This test covers the `!loop->refcount` branch in evio_timeout().
    // An active idle watcher keeps the loop alive but doesn't add to refcount
    // for timeout purposes. This should cause evio_timeout() to return 0.
    evio_idle idle;
    evio_idle_init(&idle, generic_cb);
    idle.data = &data;
    evio_idle_start(loop, &idle);
    evio_unref(loop); // Manually unref to make refcount 0 for timeout calculation.

    // This will run, call evio_timeout() which returns 0, and run the idle cb.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);

    evio_loop_free(loop);
}

TEST(test_evio_run_event_pending_no_events)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Manually set the event_pending flag, but don't queue any events.
    // This simulates a race where a wakeup was signaled, but the event
    // was processed and cleared by another thread before this loop iteration.
    atomic_store_explicit(&loop->event_pending.value, 1, memory_order_relaxed);

    // This covers the branch `if (atomic_load_explicit(&loop->event_pending.value, ...))`
    // where the condition is true, but no event is actually queued.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callbacks should fire.
    assert_int_equal(evio_pending_count(loop), 0);
    evio_loop_free(loop);
}

static void reentrant_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *counter = base->data;
    ++(*counter);

    // Make a nested call. It should be able to process events.
    evio_invoke_pending(loop);

    // Queue another event for this same watcher.
    // This will be added to the alternate pending queue and processed by the
    // outer `evio_invoke_pending`'s `for(;;)` loop after this callback returns.
    if (*counter < 2) {
        evio_feed_event(loop, base, EVIO_PREPARE);
    }
}

TEST(test_evio_invoke_pending_reentrancy)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, reentrant_cb);
    evio_prepare_start(loop, &prepare);

    size_t counter = 0;
    prepare.data = &counter;

    evio_feed_event(loop, &prepare.base, EVIO_PREPARE);

    // The single call to invoke_pending should trigger a chain of two callbacks.
    evio_invoke_pending(loop);

    assert_int_equal(counter, 2);
    assert_int_equal(evio_pending_count(loop), 0);
    assert_int_equal(evio_refcount(loop), 1); // watcher is still active

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

// Test to highlight the flaw of a non-re-entrant evio_invoke_pending.
// Without a guard, a re-entrant call can lead to deep, unbounded recursion
// instead of the intended iterative processing by the top-level call. This
// can lead to stack exhaustion in real-world scenarios. A safety limit is
// used here to prevent the test itself from crashing.
#define RECURSION_LIMIT 20
static evio_prepare prepare_flaw1, prepare_flaw2;
static size_t flaw_cb1_called = 0;
static size_t flaw_cb2_called = 0;

static void flaw_cb1(evio_loop *loop, evio_base *base, evio_mask emask)
{
    flaw_cb1_called++;
    evio_feed_event(loop, &prepare_flaw2.base, EVIO_PREPARE);
    // This re-entrant call immediately processes the new events,
    // leading to deep recursion.
    evio_invoke_pending(loop);
}

static void flaw_cb2(evio_loop *loop, evio_base *base, evio_mask emask)
{
    flaw_cb2_called++;
    if (flaw_cb1_called < RECURSION_LIMIT) {
        evio_feed_event(loop, &prepare_flaw1.base, EVIO_PREPARE);
    }
}

// This test demonstrates the recursive nature of evio_invoke_pending.
// A re-entrant call from a callback will immediately process new events,
// leading to deep recursion. A limit is used to prevent stack overflow.
TEST(test_evio_invoke_pending_recursion)
{
    flaw_cb1_called = 0;
    flaw_cb2_called = 0;

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare_init(&prepare_flaw1, flaw_cb1);
    evio_prepare_start(loop, &prepare_flaw1);

    evio_prepare_init(&prepare_flaw2, flaw_cb2);
    evio_prepare_start(loop, &prepare_flaw2);

    evio_feed_event(loop, &prepare_flaw1.base, EVIO_PREPARE);
    evio_invoke_pending(loop);

    evio_loop_free(loop);

    // evio_invoke_pending is re-entrant, so this test demonstrates the deep
    // recursion that occurs, hitting the safety limit.
    assert_int_equal(flaw_cb1_called, RECURSION_LIMIT);
    assert_int_equal(flaw_cb2_called, RECURSION_LIMIT);
}

// This test demonstrates the depth-first event processing order that results
// from the re-entrant nature of evio_invoke_pending.
static evio_prepare prepare_A, prepare_B, prepare_C;
static char execution_order[4];
static size_t execution_idx;

static void reentrant_cb_A(evio_loop *loop, evio_base *base, evio_mask emask)
{
    execution_order[execution_idx++] = 'A';

    // Queue event C
    evio_feed_event(loop, &prepare_C.base, EVIO_PREPARE);

    // Re-entrant call. Without a guard, this will process C immediately,
    // before B gets a chance.
    evio_invoke_pending(loop);
}

static void reentrant_cb_B(evio_loop *loop, evio_base *base, evio_mask emask)
{
    execution_order[execution_idx++] = 'B';
}

static void reentrant_cb_C(evio_loop *loop, evio_base *base, evio_mask emask)
{
    execution_order[execution_idx++] = 'C';
}

// This test verifies the depth-first event processing order ("ACB") that results
// from re-entrant calls to evio_invoke_pending.
TEST(test_evio_invoke_pending_depth_first_order)
{
    execution_idx = 0;
    memset(execution_order, 0, sizeof(execution_order));

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare_init(&prepare_A, reentrant_cb_A);
    evio_prepare_init(&prepare_B, reentrant_cb_B);
    evio_prepare_init(&prepare_C, reentrant_cb_C);

    evio_prepare_start(loop, &prepare_A);
    evio_prepare_start(loop, &prepare_B);
    evio_prepare_start(loop, &prepare_C);

    // Queue A and B.
    evio_feed_event(loop, &prepare_B.base, EVIO_PREPARE);
    evio_feed_event(loop, &prepare_A.base, EVIO_PREPARE);

    evio_invoke_pending(loop);
    evio_loop_free(loop);

    // The re-entrant call in cb_A processes C immediately, before B gets a
    // chance, so the expected depth-first order is "ACB".
    assert_string_equal(execution_order, "ACB");
}
