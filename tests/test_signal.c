#include "test.h"

TEST(test_evio_signal)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig;
    evio_signal_init(&sig, generic_cb, SIGUSR1);
    evio_signal_start(loop, &sig);

    // Double start should be a no-op
    evio_signal_start(loop, &sig);
    assert_int_equal(evio_refcount(loop), 1);

    // Simulate signal delivery
    evio_feed_signal(loop, SIGUSR1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb_emask, EVIO_SIGNAL);

    evio_signal_stop(loop, &sig);
    // Double stop should be a no-op
    evio_signal_stop(loop, &sig);
    assert_int_equal(evio_refcount(loop), 0);
    evio_loop_free(loop);
}

TEST(test_evio_signal_multiple_watchers)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);

    evio_signal sig2;
    evio_signal_init(&sig2, generic_cb2, SIGUSR1);

    // Start both. The sigaction should only happen once.
    evio_signal_start(loop, &sig1);
    evio_signal_start(loop, &sig2);
    assert_int_equal(evio_refcount(loop), 2);

    evio_feed_signal(loop, SIGUSR1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb2_called, 1);

    // Stop one. The sigaction should not be restored yet.
    evio_signal_stop(loop, &sig1);
    assert_int_equal(evio_refcount(loop), 1);

    // Stop the second one. This should restore the sigaction.
    evio_signal_stop(loop, &sig2);
    assert_int_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

typedef struct {
    int signum;
    pthread_barrier_t *barrier;
} signal_thread_arg;

static void *signal_raiser_thread(void *arg)
{
    signal_thread_arg *t_arg = arg;
    pthread_barrier_wait(t_arg->barrier);
    raise(t_arg->signum);
    return NULL;
}

TEST(test_evio_signal_concurrent_raise)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);
    evio_signal_start(loop, &sig1);

    evio_signal sig2;
    evio_signal_init(&sig2, generic_cb2, SIGUSR2);
    evio_signal_start(loop, &sig2);

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 3);

    pthread_t t1, t2;
    signal_thread_arg arg1 = { .signum = SIGUSR1, .barrier = &barrier };
    signal_thread_arg arg2 = { .signum = SIGUSR2, .barrier = &barrier };

    pthread_create(&t1, NULL, signal_raiser_thread, &arg1);
    pthread_create(&t2, NULL, signal_raiser_thread, &arg2);

    // Wait for threads to be ready, then unblock them.
    pthread_barrier_wait(&barrier);

    // Wait for raiser threads to finish.
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    // Both signals will have been raised. One will have set signal_pending and written to eventfd.
    // The other will only have set its own status.
    // The loop should process both.
    evio_run(loop, EVIO_RUN_ONCE);

    pthread_barrier_destroy(&barrier);

    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb2_called, 1);

    evio_signal_stop(loop, &sig1);
    evio_signal_stop(loop, &sig2);
    evio_loop_free(loop);
}

// Test the race condition where a signal's status is set, but the loop
// associated with it is different from the one processing events.
TEST(test_evio_signal_process_pending_race)
{
    reset_cb_state();

    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop1);

    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop2);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);
    evio_signal_start(loop1, &sig1);

    evio_signal sig2;
    evio_signal_init(&sig2, generic_cb2, SIGUSR2);
    evio_signal_start(loop2, &sig2);

    // Raise both signals. This sets the status for each signal struct and sets
    // the `signal_pending` flag on each respective loop.
    raise(SIGUSR1);
    raise(SIGUSR2);

    // Run loop2. It will call evio_signal_process_pending(loop2).
    // Inside, it will see its own `signal_pending` flag is set.
    // When it iterates through signals, it will process SIGUSR2 but correctly
    // skip SIGUSR1 because it belongs to loop1.
    evio_run(loop2, EVIO_RUN_ONCE);

    // Callback for loop2's watcher (sig2) should have been called.
    assert_int_equal(generic_cb2_called, 1);
    // Callback for loop1's watcher (sig1) should NOT have been called yet.
    assert_int_equal(generic_cb_called, 0);

    // Now, run loop1 to process its pending signal.
    evio_run(loop1, EVIO_RUN_ONCE);
    assert_int_equal(generic_cb_called, 1);

    evio_loop_free(loop1);
    evio_loop_free(loop2);
}

typedef struct {
    pthread_barrier_t *barrier;
    evio_loop *loop1;
    evio_loop *loop2;
    evio_signal *sig1;
    evio_signal *sig2;
} stale_status_thread_arg;

// This thread function simulates a race where a signal is handled, but before
// it's processed by the loop, the watcher is moved to another loop.
static void *stale_status_thread(void *arg)
{
    stale_status_thread_arg *t = arg;

    // 1. Thread waits for main thread to set up watcher on loop1.
    pthread_barrier_wait(t->barrier);

    // 2. Raise the signal. The handler runs, sets status=1 for SIGUSR1,
    //    and wakes up loop1.
    raise(SIGUSR1);

    // 3. Wait for main thread to stop the watcher on loop1 and restart on loop2.
    pthread_barrier_wait(t->barrier);

    return NULL;
}

TEST(test_evio_signal_process_pending_stale_status)
{
    reset_cb_state();

    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop1);

    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop2);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 2);

    stale_status_thread_arg arg = {
        .barrier = &barrier,
        .loop1 = loop1,
        .loop2 = loop2,
        .sig1 = &sig1,
    };

    // Block SIGUSR1 in the main thread to avoid a race where the signal is
    // delivered after its handler has been uninstalled by evio_signal_stop.
    // The raiser thread will inherit the default (unblocked) mask and can
    // receive and handle the signal.
    sigset_t set, oldset;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, &oldset);

    // Main thread: Start watcher on loop1.
    evio_signal_start(loop1, &sig1);

    pthread_t thread;
    assert_int_equal(pthread_create(&thread, NULL, stale_status_thread, &arg), 0);

    // Main thread: Signal thread to raise the signal.
    pthread_barrier_wait(&barrier);

    // Main thread: Immediately stop watcher on loop1 and restart on loop2.
    // The signal status for SIGUSR1 is still 1, but it's now owned by loop2.
    evio_signal_stop(loop1, &sig1);
    evio_signal_start(loop2, &sig1);

    // Main thread: Signal thread that we are done moving the watcher.
    pthread_barrier_wait(&barrier);

    // Run loop1. It was woken up by the signal, but when it processes pending
    // signals, it will see that the watcher for SIGUSR1 now belongs to loop2.
    // It will hit the `atomic_exchange_explicit(&sig->status.value, 0, ...)`
    // but the inner `for` loop will not run because `sig->list.count` is 0 for loop1.
    // This covers the target branch.
    evio_run(loop1, EVIO_RUN_ONCE);

    // No callback should have been called, as the event was for a stale watcher.
    assert_int_equal(generic_cb_called, 0);

    assert_int_equal(pthread_join(thread, NULL), 0);
    evio_loop_free(loop1);
    evio_loop_free(loop2);
    pthread_barrier_destroy(&barrier);

    // Restore the original signal mask.
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
}

TEST(test_evio_signal_pending_another_signal)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);
    evio_signal_start(loop, &sig1);

    evio_signal sig2;
    evio_signal_init(&sig2, generic_cb2, SIGUSR2);
    evio_signal_start(loop, &sig2);

    // Raise only one of the two signals being watched by the loop.
    raise(SIGUSR1);

    // Run the loop. When evio_signal_process_pending is called, it will iterate
    // over all signals. For SIGUSR1, it will find status=1 and queue an event.
    // For SIGUSR2, it will find status=0 and take the uncovered `else` path.
    evio_run(loop, EVIO_RUN_ONCE);

    // Verify only the correct callback was fired.
    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb2_called, 0);

    evio_signal_stop(loop, &sig1);
    evio_signal_stop(loop, &sig2);
    evio_loop_free(loop);
}

TEST(test_evio_signal_real_signal)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig;
    evio_signal_init(&sig, generic_cb, SIGUSR1);
    evio_signal_start(loop, &sig);

    raise(SIGUSR1);

    // The signal handler will write to the eventfd, waking up the loop.
    evio_run(loop, EVIO_RUN_ONCE);

    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb_emask, EVIO_SIGNAL);

    evio_signal_stop(loop, &sig);
    evio_loop_free(loop);
}

TEST(test_evio_signal_multiple_loops)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);
    evio_set_abort(custom_abort_handler, NULL);
    custom_abort_called = 0;

    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop1);

    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop2);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR2);
    evio_signal_start(loop1, &sig1);

    evio_signal sig2;
    evio_signal_init(&sig2, generic_cb, SIGUSR2);

    if (setjmp(abort_jmp_buf) == 0) {
        // This should abort because SIGUSR2 is already bound to loop1
        evio_signal_start(loop2, &sig2);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(custom_abort_called, 1);

    // Cleanup, this will call evio_signal_cleanup_loop
    evio_loop_free(loop1);
    evio_loop_free(loop2);

    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_signal_process_pending_skip)
{
    reset_cb_state();

    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop1);

    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop2);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);
    evio_signal_start(loop1, &sig1);

    evio_signal sig2;
    evio_signal_init(&sig2, generic_cb2, SIGUSR2);
    evio_signal_start(loop2, &sig2);

    raise(SIGUSR1);

    // loop1 should process SIGUSR1. During processing, it will iterate all
    // signals and skip SIGUSR2 because it's attached to loop2.
    evio_run(loop1, EVIO_RUN_ONCE);
    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb2_called, 0);

    // Just to check loop2 is fine and wasn't affected.
    raise(SIGUSR2);

    evio_run(loop2, EVIO_RUN_ONCE);
    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb2_called, 1);

    evio_loop_free(loop1); // This will clean up sig1
    evio_loop_free(loop2); // This will clean up sig2
}

TEST(test_evio_signal_wrong_loop)
{
    reset_cb_state();

    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop1);

    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop2);

    evio_signal sig;
    evio_signal_init(&sig, generic_cb, SIGUSR1);
    evio_signal_start(loop1, &sig);

    // Feed signal to the wrong loop, should do nothing.
    evio_feed_signal(loop2, SIGUSR1);
    evio_run(loop2, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0);

    evio_loop_free(loop1);
    evio_loop_free(loop2);
}

TEST(test_evio_signal_invalid_signum)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // feed invalid signal, should be a no-op and not crash
    evio_feed_signal(loop, -1);
    evio_feed_signal(loop, 0);
    evio_feed_signal(loop, NSIG);
    evio_feed_signal(loop, NSIG + 1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0);

    evio_loop_free(loop);
}

TEST(test_evio_signal_init_invalid_signum)
{
    evio_signal sig;
    expect_assert_failure(evio_signal_init(&sig, generic_cb, 0));
    expect_assert_failure(evio_signal_init(&sig, generic_cb, NSIG));
}

TEST(test_evio_signal_start_invalid_signum)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig;
    evio_init(&sig.base, generic_cb);

    sig.signum = 0;
    expect_assert_failure(evio_signal_start(loop, &sig));

    sig.signum = NSIG;
    expect_assert_failure(evio_signal_start(loop, &sig));

    evio_loop_free(loop);
}

TEST(test_evio_signal_stop_invalid_signum)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig;
    evio_signal_init(&sig, generic_cb, SIGUSR1);
    evio_signal_start(loop, &sig);

    sig.signum = 0;
    expect_assert_failure(evio_signal_stop(loop, &sig));

    sig.signum = NSIG;
    expect_assert_failure(evio_signal_stop(loop, &sig));

    evio_loop_free(loop);
}

TEST(test_evio_feed_signal_no_watcher)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Feed a signal for which there is no watcher.
    // This should be a no-op and not crash.
    evio_feed_signal(loop, SIGUSR1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0);

    evio_loop_free(loop);
}
