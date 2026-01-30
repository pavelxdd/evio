#include "test.h"
#include "abort.h"

#include "evio_signal.h"
#include "evio_signal_sys.h"

static struct {
    bool active;
    int err;
} evio_signal_sigaction_inject;

void evio_signal_test_inject_sigaction_fail_once(int err)
{
    evio_signal_sigaction_inject.active = true;
    evio_signal_sigaction_inject.err = err;
}

int evio_test_sigaction(int signum, const struct sigaction *act,
                        struct sigaction *oldact)
{
    if (evio_signal_sigaction_inject.active) {
        evio_signal_sigaction_inject.active = false;
        errno = evio_signal_sigaction_inject.err;
        return -1;
    }

    return sigaction(signum, act, oldact);
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

// GCOVR_EXCL_START
static void dummy_cb(evio_loop *loop, evio_base *base, evio_mask emask) {}
// GCOVR_EXCL_STOP

TEST(test_evio_signal)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig;
    evio_signal_init(&sig, generic_cb, SIGUSR1);
    sig.data = &data;
    evio_signal_start(loop, &sig);

    // Double start: no-op
    evio_signal_start(loop, &sig);
    assert_int_equal(evio_refcount(loop), 1);

    // Simulate signal delivery
    evio_feed_signal(loop, SIGUSR1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_SIGNAL);

    evio_signal_stop(loop, &sig);
    // Double stop: no-op
    evio_signal_stop(loop, &sig);
    assert_int_equal(evio_refcount(loop), 0);
    evio_loop_free(loop);
}

TEST(test_evio_signal_multiple_watchers)
{
    generic_cb_data data1 = { 0 };
    generic_cb_data data2 = { 0 };

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);
    sig1.data = &data1;

    evio_signal sig2;
    evio_signal_init(&sig2, generic_cb, SIGUSR1);
    sig2.data = &data2;

    // Start both; sigaction happens once.
    evio_signal_start(loop, &sig1);
    evio_signal_start(loop, &sig2);
    assert_int_equal(evio_refcount(loop), 2);

    evio_feed_signal(loop, SIGUSR1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data1.called, 1);
    assert_int_equal(data2.called, 1);

    // Stop one; keep sigaction.
    evio_signal_stop(loop, &sig1);
    assert_int_equal(evio_refcount(loop), 1);

    // Stop second; restore sigaction.
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
    generic_cb_data data1 = { 0 };
    generic_cb_data data2 = { 0 };

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);
    sig1.data = &data1;
    evio_signal_start(loop, &sig1);

    evio_signal sig2;
    evio_signal_init(&sig2, generic_cb, SIGUSR2);
    sig2.data = &data2;
    evio_signal_start(loop, &sig2);

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 3);

    signal_thread_arg arg1 = { .signum = SIGUSR1, .barrier = &barrier };
    signal_thread_arg arg2 = { .signum = SIGUSR2, .barrier = &barrier };

    pthread_t t1;
    pthread_create(&t1, NULL, signal_raiser_thread, &arg1);

    pthread_t t2;
    pthread_create(&t2, NULL, signal_raiser_thread, &arg2);

    // Wait for threads to be ready, then unblock them.
    pthread_barrier_wait(&barrier);

    // Wait for raiser threads to finish.
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    // Both signals will have been raised. One will have set signal_pending and written to eventfd.
    // The other will only have set its own status.
    // Loop processes both.
    evio_run(loop, EVIO_RUN_ONCE);

    pthread_barrier_destroy(&barrier);

    assert_int_equal(data1.called, 1);
    assert_int_equal(data2.called, 1);

    evio_signal_stop(loop, &sig1);
    evio_signal_stop(loop, &sig2);
    evio_loop_free(loop);
}

TEST(test_evio_signal_process_pending_race)
{
    generic_cb_data data1 = { 0 };
    generic_cb_data data2 = { 0 };

    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop1);

    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop2);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);
    sig1.data = &data1;
    evio_signal_start(loop1, &sig1);

    evio_signal sig2;
    evio_signal_init(&sig2, generic_cb, SIGUSR2);
    sig2.data = &data2;
    evio_signal_start(loop2, &sig2);

    raise(SIGUSR1);
    raise(SIGUSR2);

    evio_run(loop2, EVIO_RUN_ONCE);

    assert_int_equal(data2.called, 1);
    assert_int_equal(data1.called, 0);

    evio_run(loop1, EVIO_RUN_ONCE);
    assert_int_equal(data1.called, 1);

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

    // Wait for main thread to set up watcher on loop1.
    pthread_barrier_wait(t->barrier);

    // Raise the signal. The handler runs, sets status=1 for SIGUSR1, and wakes up loop1.
    raise(SIGUSR1);

    // Wait for main thread to stop the watcher on loop1 and restart on loop2.
    pthread_barrier_wait(t->barrier);

    return NULL;
}

TEST(test_evio_signal_process_pending_stale_status)
{
    generic_cb_data data = { 0 };

    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop1);

    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop2);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);
    sig1.data = &data;

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
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);

    sigset_t oldset;
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
    evio_run(loop1, EVIO_RUN_ONCE);

    // No callback was called, as the event was for a stale watcher.
    assert_int_equal(data.called, 0);

    assert_int_equal(pthread_join(thread, NULL), 0);
    evio_loop_free(loop1);
    evio_loop_free(loop2);
    pthread_barrier_destroy(&barrier);

    // Restore the original signal mask.
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
}

TEST(test_evio_signal_pending_another_signal)
{
    generic_cb_data data1 = { 0 };
    generic_cb_data data2 = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);
    sig1.data = &data1;
    evio_signal_start(loop, &sig1);

    evio_signal sig2;
    evio_signal_init(&sig2, generic_cb, SIGUSR2);
    sig2.data = &data2;
    evio_signal_start(loop, &sig2);

    // Raise only one of the two signals being watched by the loop.
    raise(SIGUSR1);

    evio_run(loop, EVIO_RUN_ONCE);

    assert_int_equal(data1.called, 1);
    assert_int_equal(data2.called, 0);

    evio_signal_stop(loop, &sig1);
    evio_signal_stop(loop, &sig2);
    evio_loop_free(loop);
}

TEST(test_evio_signal_real_signal)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig;
    evio_signal_init(&sig, generic_cb, SIGUSR1);
    sig.data = &data;
    evio_signal_start(loop, &sig);

    raise(SIGUSR1);

    evio_run(loop, EVIO_RUN_ONCE);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_SIGNAL);

    evio_signal_stop(loop, &sig);
    evio_loop_free(loop);
}

TEST(test_evio_signal_multiple_loops)
{
    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };
    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop1);

    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop2);

    evio_signal sig1;
    evio_signal_init(&sig1, dummy_cb, SIGUSR2);
    evio_signal_start(loop1, &sig1);

    evio_signal sig2;
    evio_signal_init(&sig2, dummy_cb, SIGUSR2);

    if (setjmp(jmp) == 0) {
        // SIGUSR2 already bound to loop1.
        evio_signal_start(loop2, &sig2);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(abort_ctx.called, 1);

    // Cleanup, this will call evio_signal_cleanup_loop
    evio_loop_free(loop1);
    evio_loop_free(loop2);

    evio_test_abort_ctx_end(&abort_ctx);
}

TEST(test_evio_signal_process_pending_skip)
{
    generic_cb_data data1 = { 0 };
    generic_cb_data data2 = { 0 };

    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop1);

    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop2);

    evio_signal sig1;
    evio_signal_init(&sig1, generic_cb, SIGUSR1);
    sig1.data = &data1;
    evio_signal_start(loop1, &sig1);

    evio_signal sig2;
    evio_signal_init(&sig2, generic_cb, SIGUSR2);
    sig2.data = &data2;
    evio_signal_start(loop2, &sig2);

    raise(SIGUSR1);

    // loop1 processes SIGUSR1; SIGUSR2 belongs to loop2.
    evio_run(loop1, EVIO_RUN_ONCE);
    assert_int_equal(data1.called, 1);
    assert_int_equal(data2.called, 0);

    // Just to check loop2 is fine and wasn't affected.
    raise(SIGUSR2);

    evio_run(loop2, EVIO_RUN_ONCE);
    assert_int_equal(data1.called, 1);
    assert_int_equal(data2.called, 1);

    evio_loop_free(loop1);
    evio_loop_free(loop2);
}

TEST(test_evio_signal_process_pending_stale_active)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int signum = SIGUSR1;
    unsigned int idx = (unsigned int)(signum - 1);
    loop->sig_active[idx >> 6] |= 1ull << (idx & 63);

    atomic_store_explicit(&loop->signal_pending.value, 1, memory_order_release);
    evio_signal_process_pending(loop);

    evio_loop_free(loop);
}

TEST(test_evio_signal_wrong_loop)
{
    generic_cb_data data = { 0 };

    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop1);

    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop2);

    evio_signal sig;
    evio_signal_init(&sig, generic_cb, SIGUSR1);
    sig.data = &data;
    evio_signal_start(loop1, &sig);

    // Feed signal to the wrong loop: no-op.
    evio_feed_signal(loop2, SIGUSR1);
    evio_run(loop2, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    evio_loop_free(loop1);
    evio_loop_free(loop2);
}

TEST(test_evio_signal_invalid_signum)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // feed invalid signal, no-op and not crash
    evio_feed_signal(loop, -1);
    evio_feed_signal(loop, 0);
    evio_feed_signal(loop, NSIG);
    evio_feed_signal(loop, NSIG + 1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    evio_loop_free(loop);
}

TEST(test_evio_signal_invalid_signum_asserts)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig;

    expect_assert_failure(evio_signal_init(&sig, dummy_cb, 0));
    expect_assert_failure(evio_signal_init(&sig, dummy_cb, NSIG));

    evio_init(&sig.base, dummy_cb);
    sig.signum = 0;
    expect_assert_failure(evio_signal_start(loop, &sig));
    sig.signum = NSIG;
    expect_assert_failure(evio_signal_start(loop, &sig));

    evio_signal_init(&sig, dummy_cb, SIGUSR1);
    evio_signal_start(loop, &sig);
    assert_true(sig.active);

    sig.signum = 0;
    expect_assert_failure(evio_signal_stop(loop, &sig));
    sig.signum = NSIG;
    expect_assert_failure(evio_signal_stop(loop, &sig));

    sig.signum = SIGUSR1;
    evio_signal_stop(loop, &sig);

    evio_loop_free(loop);
}

TEST(test_evio_feed_signal_no_watcher)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Feed a signal for which there is no watcher.
    // No-op; must not crash.
    evio_feed_signal(loop, SIGUSR1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    evio_loop_free(loop);
}

TEST(test_evio_signal_sigaction_fail)
{
    evio_signal sig = { 0 };
    expect_assert_failure(evio_signal_set(&sig, SIGKILL));
    expect_assert_failure(evio_signal_set(&sig, SIGSTOP));
}

TEST(test_evio_signal_sigaction_fail_stop)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig;
    evio_signal_init(&sig, dummy_cb, SIGUSR1);
    evio_signal_start(loop, &sig);

    evio_signal_test_inject_sigaction_fail_once(EINVAL);
    expect_assert_failure(evio_signal_stop(loop, &sig));

    evio_signal_stop(loop, &sig);
    evio_loop_free(loop);
}

TEST(test_evio_signal_sigaction_fail_cleanup)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig;
    evio_signal_init(&sig, dummy_cb, SIGUSR1);
    evio_signal_start(loop, &sig);

    evio_signal_test_inject_sigaction_fail_once(EINVAL);
    expect_assert_failure(evio_signal_cleanup_loop(loop));

    evio_signal_cleanup_loop(loop);
    evio_loop_free(loop);
}

TEST(test_evio_signal_stale_status_after_stop)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_signal sig;
    evio_signal_init(&sig, generic_cb, SIGUSR1);
    sig.data = &data;
    evio_signal_start(loop, &sig);

    // Raise the signal (sets sig->status = 1)
    raise(SIGUSR1);

    // Stop the watcher WITHOUT running the loop.
    // Clear sig->status to prevent stale delivery.
    evio_signal_stop(loop, &sig);

    // Start the watcher again
    evio_signal_start(loop, &sig);

    // Run the loop - if status was not cleared, the callback would be called
    // even though no new signal was raised.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callback was called because the signal was "consumed"
    // by the stop, and no new signal was raised after restart.
    assert_int_equal(data.called, 0);

    // Raise a new signal and verify it works
    raise(SIGUSR1);
    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(data.called, 1);

    evio_signal_stop(loop, &sig);
    evio_loop_free(loop);
}
