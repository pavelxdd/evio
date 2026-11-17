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

static void previous_signal_handler(int signum) {}

typedef struct {
    struct sigaction original;
    struct sigaction expected;
    evio_loop *loop;
    evio_signal sig[2];
    generic_cb_data data;
} signal_action_state;

static int signal_action_teardown(void **state)
{
    signal_action_state *s = *state;
    if (!s) {
        return 0;
    }

    if (s->loop) {
        evio_loop_free(s->loop);
    }
    int rc = sigaction(SIGUSR1, &s->original, NULL);
    free(s);
    *state = NULL;
    return rc;
}

static int signal_action_setup(void **state)
{
    signal_action_state *s = calloc(1, sizeof(*s));
    if (!s) {
        return -1;
    }

    struct sigaction action = {
        .sa_handler = previous_signal_handler,
        .sa_flags = SA_RESTART,
    };
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR2);
    if (sigaction(SIGUSR1, &action, &s->original)) {
        free(s);
        return -1;
    }
    *state = s;

    if (sigaction(SIGUSR1, NULL, &s->expected)) {
        signal_action_teardown(state);
        return -1;
    }

    s->loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!s->loop) {
        signal_action_teardown(state);
        return -1;
    }

    for (size_t i = 0; i < sizeof(s->sig) / sizeof(s->sig[0]); ++i) {
        evio_signal_init(&s->sig[i], generic_cb, SIGUSR1);
        s->sig[i].data = &s->data;
    }
    return 0;
}

static void assert_signal_action_equal(const struct sigaction *actual, const struct sigaction *expected)
{
    assert_true(actual->sa_handler == expected->sa_handler);
    assert_int_equal(actual->sa_flags, expected->sa_flags);
    for (int signum = 1; signum < NSIG; ++signum) {
        assert_int_equal(sigismember(&actual->sa_mask, signum), sigismember(&expected->sa_mask, signum));
    }
}

TEST(test_evio_signal_restore_action_on_stop,
     .setup_func    = signal_action_setup,
     .teardown_func = signal_action_teardown)
{
    signal_action_state *s = *state;
    evio_signal_start(s->loop, &s->sig[0]);

    struct sigaction installed;
    assert_int_equal(sigaction(SIGUSR1, NULL, &installed), 0);

    assert_int_equal(raise(SIGUSR1), 0);
    evio_signal_start(s->loop, &s->sig[0]);
    evio_run(s->loop, EVIO_RUN_NOWAIT);
    evio_invoke_pending(s->loop);
    assert_int_equal(s->data.called, 1);

    evio_signal_start(s->loop, &s->sig[1]);
    evio_signal_stop(s->loop, &s->sig[0]);

    struct sigaction active;
    assert_int_equal(sigaction(SIGUSR1, NULL, &active), 0);
    assert_signal_action_equal(&active, &installed);

    evio_signal_stop(s->loop, &s->sig[1]);

    struct sigaction restored;
    assert_int_equal(sigaction(SIGUSR1, NULL, &restored), 0);
    assert_signal_action_equal(&restored, &s->expected);
    assert_int_equal(raise(SIGUSR1), 0);
}

TEST(test_evio_signal_restore_action_on_free,
     .setup_func    = signal_action_setup,
     .teardown_func = signal_action_teardown)
{
    signal_action_state *s = *state;
    evio_signal_start(s->loop, &s->sig[0]);
    assert_int_equal(raise(SIGUSR1), 0);
    evio_run(s->loop, EVIO_RUN_NOWAIT);
    assert_int_equal(s->data.called, 0);
    assert_int_equal(evio_pending_count(s->loop), 1);

    evio_loop_free(s->loop);
    s->loop = NULL;
    assert_int_equal(s->sig[0].pending, 0);
    assert_int_equal(s->data.called, 0);

    struct sigaction restored;
    assert_int_equal(sigaction(SIGUSR1, NULL, &restored), 0);
    assert_signal_action_equal(&restored, &s->expected);
    assert_int_equal(raise(SIGUSR1), 0);
}

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
    evio_invoke_pending(loop);

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
    evio_invoke_pending(loop2);

    assert_int_equal(data2.called, 1);
    assert_int_equal(data1.called, 0);

    evio_run(loop1, EVIO_RUN_ONCE);
    evio_invoke_pending(loop1);
    assert_int_equal(data1.called, 1);

    evio_loop_free(loop1);
    evio_loop_free(loop2);
}

typedef struct {
    pthread_barrier_t *barrier;
    int rc;
} stale_status_thread_arg;

static void *stale_status_thread(void *arg)
{
    stale_status_thread_arg *t = arg;

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    t->rc = pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    if (!t->rc) {
        t->rc = raise(SIGUSR1);
    }

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
    };

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);

    sigset_t oldset;
    pthread_sigmask(SIG_BLOCK, &set, &oldset);

    // Main thread: Start watcher on loop1.
    evio_signal_start(loop1, &sig1);

    pthread_t thread;
    assert_int_equal(pthread_create(&thread, NULL, stale_status_thread, &arg), 0);

    // The signal handler must finish before the watcher moves to loop2.
    pthread_barrier_wait(&barrier);

    bool signal_pending = atomic_load_explicit(&loop1->signal_pending.value, memory_order_acquire);

    evio_signal_stop(loop1, &sig1);
    evio_signal_start(loop2, &sig1);

    evio_run(loop1, EVIO_RUN_ONCE);
    evio_invoke_pending(loop1);

    assert_int_equal(pthread_join(thread, NULL), 0);
    evio_loop_free(loop1);
    evio_loop_free(loop2);
    pthread_barrier_destroy(&barrier);

    // Restore the original signal mask.
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);

    assert_int_equal(arg.rc, 0);
    assert_true(signal_pending);
    assert_int_equal(data.called, 0);
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
    evio_invoke_pending(loop);

    assert_int_equal(data1.called, 1);
    assert_int_equal(data2.called, 0);

    evio_signal_stop(loop, &sig1);
    evio_signal_stop(loop, &sig2);
    evio_loop_free(loop);
}

TEST(test_evio_signal_real_signal)
{
    const int flags[] = { EVIO_RUN_ONCE, EVIO_RUN_NOWAIT };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        generic_cb_data data = { 0 };
        evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
        assert_non_null(loop);

        evio_signal sig;
        evio_signal_init(&sig, generic_cb, SIGUSR1);
        sig.data = &data;
        evio_signal_start(loop, &sig);

        raise(SIGUSR1);

        evio_run(loop, flags[i]);
        assert_int_equal(data.called, 0);
        assert_int_equal(evio_pending_count(loop), 1);

        evio_run(loop, EVIO_RUN_NOWAIT);

        assert_int_equal(data.called, 1);
        assert_int_equal(data.emask, EVIO_SIGNAL);
        assert_int_equal(evio_pending_count(loop), 0);

        evio_signal_stop(loop, &sig);
        evio_loop_free(loop);
    }
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
    evio_invoke_pending(loop1);
    assert_int_equal(data1.called, 1);
    assert_int_equal(data2.called, 0);

    // Just to check loop2 is fine and wasn't affected.
    raise(SIGUSR2);

    evio_run(loop2, EVIO_RUN_ONCE);
    evio_invoke_pending(loop2);
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
    evio_invoke_pending(loop);

    // No callback was called because the signal was "consumed"
    // by the stop, and no new signal was raised after restart.
    assert_int_equal(data.called, 0);

    // Raise a new signal and verify it works
    raise(SIGUSR1);
    evio_run(loop, EVIO_RUN_ONCE);
    evio_invoke_pending(loop);
    assert_int_equal(data.called, 1);

    evio_signal_stop(loop, &sig);
    evio_loop_free(loop);
}
