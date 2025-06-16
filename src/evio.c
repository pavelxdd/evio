#include <errno.h> // IWYU pragma: keep
#include <unistd.h>
#include <fcntl.h>
#include <setjmp.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "evio_core.h"

#define TEST(func) static void func(const char *__unit)

// GCOVR_EXCL_START
static void assert_expr(const char *unit, bool result,
                        const char *const expression,
                        const char *const file, const int line)
{
    if (__evio_likely(result)) {
        return;
    }

    fprintf(stderr, "%s:%d: unit test '%s' assertion failed: (%s)\n",
            file, line, unit, expression);
    fflush(stderr);
    abort();
}
// GCOVR_EXCL_STOP

#define assert_true(x) assert_expr(__unit, !!(x), #x, __FILE__, __LINE__)
#define assert_false(x) assert_true(!(x))

#define assert_null(x) assert_true((x) == NULL)
#define assert_not_null(x) assert_true((x) != NULL)

#define assert_equal(a, b) assert_true((a) == (b))
#define assert_not_equal(a, b) assert_true((a) != (b))

#define assert_str_equal(a, b) assert_true(strcmp(a, b) == 0)
#define assert_str_not_equal(a, b) assert_true(strcmp(a, b) != 0)

#define assert_strn_equal(a, b, n) assert_true(strncmp(a, b, n) == 0)
#define assert_strn_not_equal(a, b, n) assert_true(strncmp(a, b, n) != 0)

#define assert_mem_equal(a, b, n) assert_true(memcmp(a, b, n) == 0)
#define assert_mem_not_equal(a, b, n) assert_true(memcmp(a, b, n) != 0)

// =============================================================================
// Test Callbacks and State
// =============================================================================

static int generic_cb_called = 0;
static evio_mask generic_cb_emask = 0;

static void generic_cb(evio_loop *loop, evio_base *w, evio_mask emask)
{
    (void)loop;
    (void)w;
    generic_cb_called++;
    generic_cb_emask = emask;
}

static void reset_cb_state(void)
{
    generic_cb_called = 0;
    generic_cb_emask = 0;
}

// A callback that reads from the fd to clear the event state.
static void read_and_count_cb(evio_loop *loop, evio_base *w, evio_mask emask)
{
    (void)loop;
    (void)emask;
    char buf[1];
    int fd = (int)(intptr_t)w->data;
    ssize_t n = read(fd, buf, sizeof(buf));
    (void)n; // Ignore result, we just want to clear the pipe.
    generic_cb_called++;
}

// =============================================================================
// Abort Testing Infrastructure
// =============================================================================

static jmp_buf abort_jmp_buf;
static int custom_abort_called = 0;

static FILE *custom_abort_handler(const char *restrict file, int line,
                                  const char *restrict func,
                                  const char *restrict format, va_list ap)
{
    (void)file;
    (void)line;
    (void)func;
    (void)format;
    (void)ap;

    custom_abort_called++;
    longjmp(abort_jmp_buf, 1);
    return NULL; // Unreachable
}

// =============================================================================
// Allocation Tests
// =============================================================================

TEST(test_evio_malloc)
{
    for (size_t i = 1; i < 100; ++i) {
        void *ptr = evio_malloc(i);
        assert_not_null(ptr);
        evio_free(ptr);
    }
}

TEST(test_evio_calloc)
{
    for (size_t i = 1; i < 10; ++i) {
        for (size_t j = 1; j < 10; ++j) {
            char *ptr = evio_calloc(i, j);
            assert_not_null(ptr);
            for (size_t k = 0; k < i * j; ++k) {
                assert_equal(ptr[k], 0);
            }
            evio_free(ptr);
        }
    }
}

TEST(test_evio_realloc)
{
    void *ptr = evio_realloc(NULL, 1);
    assert_not_null(ptr);
    ptr = evio_realloc(ptr, 100);
    assert_not_null(ptr);
    ptr = evio_realloc(ptr, 1);
    assert_not_null(ptr);
    evio_free(ptr);
}

TEST(test_evio_reallocarray)
{
    void *ptr = evio_reallocarray(NULL, 1, 1);
    assert_not_null(ptr);
    ptr = evio_reallocarray(ptr, 10, 10);
    assert_not_null(ptr);
    ptr = evio_reallocarray(ptr, 1, 1);
    assert_not_null(ptr);
    evio_free(ptr);
}

static size_t custom_alloc_count = 0;
static void *custom_realloc(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    custom_alloc_count++;

    if (size) {
        return realloc(ptr, size);
    }

    free(ptr);
    return NULL;
}

TEST(test_evio_custom_allocator)
{
    // Save original allocator
    void *orig_ctx;
    evio_realloc_cb orig_cb = evio_get_allocator(&orig_ctx);

    custom_alloc_count = 0;
    evio_set_allocator(custom_realloc, NULL);

    evio_realloc_cb allocator = evio_get_allocator(NULL);
    assert_equal(allocator, custom_realloc);

    void *ptr = evio_malloc(10);
    assert_not_null(ptr);
    assert_equal(custom_alloc_count, 1);

    ptr = evio_realloc(ptr, 20);
    assert_not_null(ptr);
    assert_equal(custom_alloc_count, 2);

    evio_free(ptr);
    assert_equal(custom_alloc_count, 3);

    // Restore original allocator
    evio_set_allocator(orig_cb, orig_ctx);

    ptr = evio_malloc(10);
    assert_not_null(ptr);
    evio_free(ptr);
    assert_equal(custom_alloc_count, 3); // should not have increased
}

TEST(test_evio_alloc_overflow)
{
    evio_abort_cb old_abort = evio_get_abort();
    evio_set_abort(custom_abort_handler);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        void *p = evio_calloc(SIZE_MAX, SIZE_MAX);
        evio_free(p);
        assert_true(0); // Should not be reached
    }
    assert_equal(custom_abort_called, 1);

    custom_abort_called = 0;
    if (setjmp(abort_jmp_buf) == 0) {
        void *p = evio_reallocarray(NULL, SIZE_MAX, SIZE_MAX);
        evio_free(p);
        assert_true(0); // Should not be reached
    }
    assert_equal(custom_abort_called, 1);

    evio_set_abort(old_abort);
}

// =============================================================================
// Loop and Utility Tests
// =============================================================================

TEST(test_evio_ref_unref)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    assert_equal(evio_refcount(loop), 0);

    evio_ref(loop);
    assert_equal(evio_refcount(loop), 1);

    evio_ref(loop);
    assert_equal(evio_refcount(loop), 2);

    evio_unref(loop);
    assert_equal(evio_refcount(loop), 1);

    evio_unref(loop);
    assert_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_userdata)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);
    assert_null(evio_get_userdata(loop));

    int x = 42;
    evio_set_userdata(loop, &x);
    assert_equal(evio_get_userdata(loop), &x);
    assert_equal(*(int *)evio_get_userdata(loop), 42);

    evio_set_userdata(loop, NULL);
    assert_null(evio_get_userdata(loop));

    evio_loop_free(loop);
}

TEST(test_evio_loop_uring)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_not_null(loop);

    // Just run it once to make sure nothing crashes.
    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_loop_free(loop);
}

static int break_one_cb_called = 0;

static void break_one_cb(evio_loop *loop, evio_base *w, evio_mask emask)
{
    (void)w;
    (void)emask;
    break_one_cb_called++;
    evio_break(loop, EVIO_BREAK_ONE);
}

static int break_all_cb_called = 0;
static evio_idle break_all_watcher;

static void break_all_cb(evio_loop *loop, evio_base *w, evio_mask emask)
{
    (void)w;
    (void)emask;
    break_all_cb_called++;
    evio_break(loop, EVIO_BREAK_ALL);
}

static int nested_run_trigger_cb_called = 0;

static void nested_run_trigger_cb(evio_loop *loop, evio_base *w, evio_mask emask)
{
    (void)emask;
    nested_run_trigger_cb_called++;

    const char *__unit = w->data;

    // Start the watcher that will break out of all loops. Using an idle watcher
    // ensures the nested loop doesn't block, as it forces a zero timeout.
    evio_idle_start(loop, &break_all_watcher);

    // This nested run will be broken by break_all_cb
    evio_run(loop, EVIO_RUN_DEFAULT);

    // The nested run has returned. The outer loop will break in its next check.
    assert_equal(evio_break_state(loop), EVIO_BREAK_ALL);
}

TEST(test_evio_break)
{
    // Part 1: Test EVIO_BREAK_ONE
    break_one_cb_called = 0;
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    evio_prepare prepare_one;
    evio_prepare_init(&prepare_one, break_one_cb);
    evio_prepare_start(loop, &prepare_one);

    // evio_run should execute one iteration, then break.
    int active = evio_run(loop, EVIO_RUN_DEFAULT);
    assert_equal(break_one_cb_called, 1);
    assert_true(active);

    // Running again should do the same.
    active = evio_run(loop, EVIO_RUN_DEFAULT);
    assert_equal(break_one_cb_called, 2);
    assert_true(active);

    evio_prepare_stop(loop, &prepare_one);
    evio_loop_free(loop);

    // Part 2: Test EVIO_BREAK_ALL with nested loops
    nested_run_trigger_cb_called = 0;
    break_all_cb_called = 0;
    loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    evio_timer timer;
    evio_timer_init(&timer, nested_run_trigger_cb, 0); // one-shot timer
    timer.data = (void *)__unit;
    evio_idle_init(&break_all_watcher, break_all_cb);

    evio_timer_start(loop, &timer, 0); // Fire immediately

    // This run will enter a nested loop, which will be broken by EVIO_BREAK_ALL,
    // which should propagate and break this outer loop too.
    active = evio_run(loop, EVIO_RUN_DEFAULT);

    assert_equal(nested_run_trigger_cb_called, 1);
    assert_equal(break_all_cb_called, 1);
    assert_true(active); // An idle watcher is still active.

    // The break state is EVIO_BREAK_ALL upon exiting the run.
    assert_equal(evio_break_state(loop), EVIO_BREAK_ALL);

    // Stop the watcher that caused the break. Now refcount is 0.
    evio_idle_stop(loop, &break_all_watcher);
    assert_equal(evio_refcount(loop), 0);

    // Running again should exit immediately as there are no active watchers.
    active = evio_run(loop, EVIO_RUN_DEFAULT);
    assert_false(active);
    assert_equal(nested_run_trigger_cb_called, 1); // Should not have been called again
    assert_equal(break_all_cb_called, 1);  // Should not have been called again

    evio_loop_free(loop);
}

TEST(test_evio_clear_pending)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, generic_cb);
    evio_prepare_start(loop, &prepare);

    assert_equal(evio_pending_count(loop), 0);

    // Queue an event manually
    evio_feed_event(loop, &prepare.base, EVIO_PREPARE);
    assert_equal(evio_pending_count(loop), 1);

    // Clear it
    evio_clear_pending(loop, &prepare.base);
    assert_equal(evio_pending_count(loop), 0);

    // Since the pending event was cleared, invoking pending events
    // should do nothing.
    evio_invoke_pending(loop);
    assert_equal(generic_cb_called, 0);

    // However, running the loop will trigger the prepare watcher normally.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

TEST(test_evio_strerror_valid)
{
    char buf[EVIO_STRERROR_SIZE];
    char *err_str = evio_strerror(EAGAIN, buf, sizeof(buf));
    assert_equal(err_str, buf);
    assert_str_not_equal(err_str, "");

    err_str = evio_strerror(EINVAL, buf, sizeof(buf));
    assert_equal(err_str, buf);
    assert_str_not_equal(err_str, "");

    // Test with macro
    const char *macro_err_str = EVIO_STRERROR(EPERM);
    assert_str_not_equal(macro_err_str, "");
}

TEST(test_evio_strerror_invalid)
{
    char buf[EVIO_STRERROR_SIZE];
    // Use a large number that is likely not a valid errno
    char *err_str = evio_strerror(99999, buf, sizeof(buf));
    assert_equal(err_str, buf);

#ifdef __GLIBC__
    assert_str_equal(err_str, "Unknown error 99999");
#else
    assert_str_equal(err_str, "No error information");
#endif
}

TEST(test_evio_strerror_erange)
{
    char buf[18];
    // Use a large number that is likely not a valid errno
    char *err_str = evio_strerror(99999, buf, sizeof(buf));
    assert_equal(err_str, buf);

    assert_str_equal(err_str, "Unknown error 999");
}

TEST(test_evio_utils_abort_custom)
{
    evio_abort_cb old_abort = evio_get_abort();
    evio_set_abort(custom_abort_handler);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        EVIO_ABORT("Testing custom abort");
    }
    assert_equal(custom_abort_called, 1);

    evio_set_abort(old_abort);
}

TEST(test_evio_core_requeue)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_prepare p;
    evio_prepare_init(&p, generic_cb);
    evio_prepare_start(loop, &p);

    // Queue event twice for same watcher before invoking
    evio_feed_event(loop, &p.base, EVIO_PREPARE);
    assert_equal(evio_pending_count(loop), 1);
    evio_feed_event(loop, &p.base, EVIO_PREPARE);
    assert_equal(evio_pending_count(loop), 1);

    evio_invoke_pending(loop);
    assert_equal(generic_cb_called, 1);

    evio_prepare_stop(loop, &p);
    evio_loop_free(loop);
}

// This test is designed to cover edge cases in fd management,
// including the logic in evio_flush_fd_change/error and evio_invalidate_fd.
TEST(test_evio_poll_resizing_and_invalidation)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_poll polls[5];
    int fds[5][2];

    // 1. Start 3 watchers
    for (int i = 0; i < 3; i++) {
        assert_equal(pipe(fds[i]), 0);
        evio_poll_init(&polls[i], generic_cb, fds[i][0], EVIO_READ);
        evio_poll_start(loop, &polls[i]);
    }
    assert_equal(loop->refcount, 3);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process changes

    // 2. Stop a watcher in the middle. This will trigger list-shrinking logic.
    evio_poll_stop(loop, &polls[1]);
    assert_equal(loop->refcount, 2);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process changes

    // 3. Write to the remaining active pipes, ensure they work.
    reset_cb_state();
    assert_equal(write(fds[0][1], "x", 1), 1);
    assert_equal(write(fds[2][1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 2);

    // 4. Invalidate the stopped watcher's fd by closing it, then stop another watcher.
    // This tests the evio_invalidate_fd path.
    close(fds[1][0]);
    close(fds[1][1]);
    evio_poll_stop(loop, &polls[0]);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(loop->refcount, 1);

    // 5. Cleanup
    evio_poll_stop(loop, &polls[2]);
    close(fds[0][0]);
    close(fds[0][1]);
    close(fds[2][0]);
    close(fds[2][1]);

    evio_loop_free(loop);
}

// =============================================================================
// Timer Tests
// =============================================================================

TEST(test_evio_timer)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    evio_timer timer;
    evio_timer_init(&timer, generic_cb, 0);
    evio_timer_start(loop, &timer, 0); // Start with 0 timeout to fire immediately

    assert_equal(evio_refcount(loop), 1);
    assert_equal(generic_cb_called, 0);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_equal(generic_cb_emask, EVIO_TIMER);

    // Timer should be stopped now (one-shot)
    assert_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_repeat)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    evio_timer timer;
    evio_timer_init(&timer, generic_cb, 1);
    evio_timer_start(loop, &timer, 0);

    assert_equal(evio_refcount(loop), 1);

    evio_run(loop, EVIO_RUN_ONCE);
    assert_equal(generic_cb_called, 1);
    assert_equal(evio_refcount(loop), 1); // Should still be active

    evio_run(loop, EVIO_RUN_ONCE);
    assert_equal(generic_cb_called, 2);

    evio_timer_stop(loop, &timer);
    assert_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_timer_again_and_remaining)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    evio_timer timer;
    // repeating timer, 10 seconds
    evio_timer_init(&timer, generic_cb, EVIO_TIME_FROM_SEC(10));
    // Start with a timeout larger than the coarse clock resolution to make the test stable.
    evio_timer_start(loop, &timer, EVIO_TIME_FROM_MSEC(50));

    assert_true(evio_timer_remaining(loop, &timer) > 0);

    // On a busy CI server, the single run might not be enough due to clock granularity.
    // We loop until the timer fires, with a safety break.
    int loops = 0;
    while (generic_cb_called == 0 && loops < 10) {
        evio_run(loop, EVIO_RUN_ONCE);
        loops++;
    }
    assert_equal(generic_cb_called, 1);
    assert_equal(generic_cb_emask, EVIO_TIMER);
    reset_cb_state();

    // Timer is repeating, so it's rescheduled for 10s from now.
    // The remaining time should be close to 10s.
    assert_true(evio_timer_remaining(loop, &timer) <= EVIO_TIME_FROM_SEC(10));

    // Restart it with 'again'
    evio_timer_again(loop, &timer);
    assert_equal(generic_cb_called, 0);

    // It should be rescheduled for another 10s from the *new* current time
    assert_true(evio_timer_remaining(loop, &timer) <= EVIO_TIME_FROM_SEC(10));

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 0); // should not have fired yet

    evio_timer_stop(loop, &timer);
    assert_equal(evio_timer_remaining(loop, &timer), 0);
    assert_equal(evio_refcount(loop), 0);

    evio_loop_free(loop);
}

#define MANY_TIMERS 200
TEST(test_evio_timer_many)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    evio_timer timers[MANY_TIMERS];
    for (int i = 0; i < MANY_TIMERS; i++) {
        // Use small, slightly varied timeouts to stress heap
        evio_timer_init(&timers[i], generic_cb, 0);
        evio_timer_start(loop, &timers[i], EVIO_TIME_FROM_MSEC(i + 1));
    }

    assert_equal(evio_refcount(loop), MANY_TIMERS);

    // Run until all timers have fired
    while (evio_refcount(loop) > 0) {
        evio_run(loop, EVIO_RUN_ONCE);
    }

    assert_equal(generic_cb_called, MANY_TIMERS);
    evio_loop_free(loop);
}

TEST(test_evio_timer_fast_repeat)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    evio_timer timer;
    // A timer that repeats every nanosecond.
    evio_timer_init(&timer, generic_cb, 1);
    evio_timer_start(loop, &timer, 0);

    evio_run(loop, EVIO_RUN_ONCE);
    assert_equal(generic_cb_called, 1);

    // It should have been rescheduled.
    assert_equal(evio_refcount(loop), 1);

    evio_timer_stop(loop, &timer);
    evio_loop_free(loop);
}

// =============================================================================
// Poll Tests
// =============================================================================

TEST(test_evio_poll)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    int fds[2];
    assert_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    assert_equal(evio_refcount(loop), 1);

    // Write to the pipe to trigger a read event
    assert_equal(write(fds[1], "x", 1), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_READ);

    evio_poll_stop(loop, &io);
    assert_equal(evio_refcount(loop), 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_change)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    int fds1[2], fds2[2];
    assert_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1), 0);
    assert_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds1[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Trigger on first pipe
    assert_equal(write(fds1[1], "a", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_READ);
    reset_cb_state();

    // Change to watch for write on the same fd
    char buf[1];
    assert_equal(read(fds1[0], buf, 1), 1); // clear pipe
    evio_poll_change(loop, &io, fds1[0], EVIO_WRITE);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_WRITE);
    reset_cb_state();

    // Change to watch for read on the second pipe
    evio_poll_change(loop, &io, fds2[0], EVIO_READ);
    assert_equal(write(fds1[1], "b", 1), 1); // write to old pipe
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 0); // should not trigger

    assert_equal(write(fds2[1], "c", 1), 1); // write to new pipe
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_READ);
    reset_cb_state();

    evio_poll_stop(loop, &io);
    close(fds1[0]);
    close(fds1[1]);
    close(fds2[0]);
    close(fds2[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_change_stop)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int fds[2];
    assert_equal(pipe(fds), 0);
    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);
    assert_equal(evio_refcount(loop), 1);

    // Change with emask=0 should stop the watcher.
    evio_poll_change(loop, &io, fds[0], 0);
    assert_equal(evio_refcount(loop), 0);
    assert_false(io.active);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_feed_fd)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    int fds[2];
    assert_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Feed an event manually for the fd.
    evio_feed_fd_event(loop, fds[0], EVIO_READ);

    // Run and check that the callback was called.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_READ);

    reset_cb_state();

    // Feed an error manually for the fd.
    evio_feed_fd_error(loop, fds[0]);

    // This should stop the watcher and call the callback with an error.
    evio_invoke_pending(loop);
    assert_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_ERROR);
    assert_false(io.active);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

#define MANY_FDS 100
TEST(test_evio_poll_many_events)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    evio_poll polls[MANY_FDS];
    int fds[MANY_FDS][2];

    // 1. Setup phase: initialize and start all watchers.
    for (int i = 0; i < MANY_FDS; i++) {
        assert_equal(pipe(fds[i]), 0);
        // Use the read_and_count_cb to consume the event
        evio_poll_init(&polls[i], read_and_count_cb, fds[i][0], EVIO_READ);
        // Store the read-end of the pipe in the watcher's data field
        polls[i].base.data = (void *)(intptr_t)fds[i][0];
        evio_poll_start(loop, &polls[i]);
    }

    // 2. Flush phase: run the loop once to ensure evio_poll_update() runs
    // and all fds are added to epoll. We don't expect events yet.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 0);

    // 3. Event generation phase: write to all pipes to make them readable.
    for (int i = 0; i < MANY_FDS; i++) {
        assert_equal(write(fds[i][1], "x", 1), 1);
    }

    // 4. Processing phase: drain all pending events from the loop.
    // epoll may not return all 100 events in one go.
    int loops = 0;
    while (generic_cb_called < MANY_FDS && loops < MANY_FDS * 2) {
        evio_run(loop, EVIO_RUN_NOWAIT);
        loops++;
    }
    assert_equal(generic_cb_called, MANY_FDS);

    // 5. Cleanup phase.
    for (int i = 0; i < MANY_FDS; i++) {
        evio_poll_stop(loop, &polls[i]);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_not_null(loop);

#ifdef EVIO_IO_URING
    if (!loop->iou) {
        fprintf(stdout, "      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
#else
    fprintf(stdout, "      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    int fds1[2], fds2[2];
    assert_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1), 0);
    assert_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds1[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Trigger on first pipe
    assert_equal(write(fds1[1], "a", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    reset_cb_state();

    // Change to watch for write on the same fd
    char buf[1];
    assert_equal(read(fds1[0], buf, 1), 1); // clear pipe
    evio_poll_change(loop, &io, fds1[0], EVIO_WRITE);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    reset_cb_state();

    // Change to watch for read on the second pipe
    evio_poll_change(loop, &io, fds2[0], EVIO_READ);
    assert_equal(write(fds2[1], "c", 1), 1); // write to new pipe
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    reset_cb_state();

    evio_poll_stop(loop, &io);
    close(fds1[0]);
    close(fds1[1]);
    close(fds2[0]);
    close(fds2[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_eperm_error)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_not_null(loop);

#ifdef EVIO_IO_URING
    if (!loop->iou) {
        fprintf(stdout, "      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
#else
    fprintf(stdout, "      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    int fd = open("/dev/null", O_RDWR);
    assert_true(fd >= 0);

    evio_poll poll;
    evio_poll_init(&poll, generic_cb, fd, EVIO_READ | EVIO_WRITE);
    evio_poll_start(loop, &poll);

    // The first run will submit an io_uring epoll_ctl, which will fail with EPERM.
    // The library should queue an fd error.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // The callback should have been invoked with a poll event because of the error.
    assert_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_POLL);
    assert_true(generic_cb_emask & EVIO_READ);
    assert_true(generic_cb_emask & EVIO_WRITE);
    assert_false(generic_cb_emask & EVIO_ERROR);

    assert_true(poll.active);
    assert_equal(evio_refcount(loop), 1);

    evio_poll_stop(loop, &poll);
    close(fd);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_eexist_error)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_not_null(loop);

#ifdef EVIO_IO_URING
    if (!loop->iou) {
        fprintf(stdout, "      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
#else
    fprintf(stdout, "      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    int fds[2];
    assert_equal(pipe(fds), 0);

    // Manually add the fd to the epoll set.
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0 };
    assert_equal(epoll_ctl(loop->fd, EPOLL_CTL_ADD, fds[0], &ev), 0);

    // Now, have evio start a watcher. It will try to ADD via uring, fail with EEXIST,
    // and should recover by retrying with MOD via uring.
    evio_poll poll;
    evio_poll_init(&poll, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &poll);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process the change

    // The watcher should be active and working.
    assert_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);

    evio_poll_stop(loop, &poll);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_enoent_error)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_not_null(loop);

#ifdef EVIO_IO_URING
    if (!loop->iou) {
        fprintf(stdout, "      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
#else
    fprintf(stdout, "      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    int fds[2];
    assert_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll poll;
    evio_poll_init(&poll, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &poll);
    evio_run(loop, EVIO_RUN_NOWAIT); // Add fd via uring

    // Manually remove the fd from epoll to create an inconsistent state.
    struct epoll_event ev = { 0 };
    assert_equal(epoll_ctl(loop->fd, EPOLL_CTL_DEL, fds[0], &ev), 0);

    // Now, change the watcher. It will try to MOD, fail with ENOENT,
    // and should recover by using ADD.
    evio_poll_change(loop, &poll, fds[0], EVIO_READ | EVIO_WRITE);
    // This run should process the change, recover from the ENOENT error by
    // re-adding the fd, and then receive the write-ready event.
    evio_run(loop, EVIO_RUN_ONCE);
    assert_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_WRITE);

    evio_poll_stop(loop, &poll);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

#define MANY_URING_EVENTS 1024
TEST(test_evio_poll_uring_many_events)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_not_null(loop);

#ifdef EVIO_IO_URING
    if (!loop->iou) {
        fprintf(stdout, "      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
#else
    fprintf(stdout, "      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    evio_poll polls[MANY_URING_EVENTS];
    int fds[MANY_URING_EVENTS][2];

    // 1. Setup phase: initialize and start all watchers.
    // This will repeatedly fill and flush the io_uring submission queue.
    for (int i = 0; i < MANY_URING_EVENTS; i++) {
        assert_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]), 0);
        evio_poll_init(&polls[i], read_and_count_cb, fds[i][0], EVIO_READ);
        polls[i].base.data = (void *)(intptr_t)fds[i][0];
        evio_poll_start(loop, &polls[i]);
    }

    // 2. Flush phase: ensure all initial ADD operations are processed.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 0);

    // 3. Event generation phase: write to all sockets to make them readable.
    for (int i = 0; i < MANY_URING_EVENTS; i++) {
        assert_equal(write(fds[i][1], "x", 1), 1);
    }

    // 4. Processing phase: drain all pending events from the loop.
    // epoll may not return all events in one go.
    int loops = 0;
    while (generic_cb_called < MANY_URING_EVENTS && loops < MANY_URING_EVENTS * 2) {
        evio_run(loop, EVIO_RUN_NOWAIT);
        loops++;
    }
    assert_equal(generic_cb_called, MANY_URING_EVENTS);

    // 5. Cleanup phase.
    for (int i = 0; i < MANY_URING_EVENTS; i++) {
        evio_poll_stop(loop, &polls[i]);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    evio_loop_free(loop);
}

// This test covers the EPERM error path in evio_poll_update, which happens
// when trying to poll a file descriptor that doesn't support polling (e.g., a regular file).
TEST(test_evio_poll_eperm_error)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    int fd = open("/dev/null", O_RDWR);
    assert_true(fd >= 0);

    evio_poll poll;
    // Watch for both read and write, as /dev/null is always ready for both.
    evio_poll_init(&poll, generic_cb, fd, EVIO_READ | EVIO_WRITE);
    evio_poll_start(loop, &poll); // This will queue a change.

    // The first run will call epoll_ctl, which will fail with EPERM.
    // The library should treat this as the fd being permanently ready.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // The callback should have been invoked with a normal poll event.
    assert_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_POLL);
    assert_true(generic_cb_emask & EVIO_READ);
    assert_true(generic_cb_emask & EVIO_WRITE);
    assert_false(generic_cb_emask & EVIO_ERROR);

    // The watcher should still be active.
    assert_true(poll.active);
    assert_equal(evio_refcount(loop), 1);

    evio_poll_stop(loop, &poll);
    close(fd);
    evio_loop_free(loop);
}

TEST(test_evio_poll_eexist_error)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int fds[2];
    assert_equal(pipe(fds), 0);

    // Manually add the fd to the epoll set to create an inconsistent state.
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0 };
    assert_equal(epoll_ctl(loop->fd, EPOLL_CTL_ADD, fds[0], &ev), 0);

    // Now, have evio start a watcher. It will try to ADD, fail with EEXIST,
    // and should recover by using MOD.
    evio_poll poll;
    evio_poll_init(&poll, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &poll);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process the change

    // The watcher should be active and working.
    assert_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);

    evio_poll_stop(loop, &poll);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

// =============================================================================
// EventFD Tests
// =============================================================================

TEST(test_evio_eventfd_eagain)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_eventfd_init(loop);
    int fd = loop->event.fd;

    // Set the eventfd counter to a value that will cause the next write(1) to fail.
    uint64_t val = UINT64_MAX - 1;
    assert_equal(write(fd, &val, sizeof(val)), sizeof(val));

    // Manually allow eventfd writes to simulate the state inside evio_run
    atomic_store_explicit(&loop->eventfd_allow.value, 1, memory_order_relaxed);

    // This call to evio_eventfd_write will first attempt a write(1), which fails with EAGAIN.
    // It will then read() the counter (getting UINT64_MAX - 1 and resetting it to 0),
    // and then successfully write(1).
    evio_eventfd_write(loop);

    // The final value in the eventfd should be 1.
    uint64_t result_val;
    assert_equal(read(fd, &result_val, sizeof(result_val)), sizeof(result_val));
    assert_equal(result_val, 1);

    evio_loop_free(loop);
}

// =============================================================================
// Async Tests
// =============================================================================

typedef struct {
    evio_loop *loop;
    evio_async *async;
} thread_arg;

static void *thread_func(void *arg)
{
    thread_arg *t_arg = arg;
    evio_async_send(t_arg->loop, t_arg->async);
    return NULL;
}

TEST(test_evio_async)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    evio_async async;
    evio_async_init(&async, generic_cb);
    evio_async_start(loop, &async);

    pthread_t thread;
    thread_arg arg = { .loop = loop, .async = &async };
    assert_equal(pthread_create(&thread, NULL, thread_func, &arg), 0);

    // This will block until the eventfd is written to by the other thread
    evio_run(loop, EVIO_RUN_ONCE);
    assert_equal(generic_cb_called, 1);
    assert_equal(generic_cb_emask, EVIO_ASYNC);

    assert_equal(pthread_join(thread, NULL), 0);

    evio_async_stop(loop, &async);
    evio_loop_free(loop);
}

TEST(test_evio_async_pending)
{
    evio_async async;
    evio_async_init(&async, generic_cb);
    assert_false(evio_async_pending(&async));

    // Simulate send
    atomic_store_explicit(&async.status.value, 1, memory_order_release);
    assert_true(evio_async_pending(&async));
}

// =============================================================================
// Once Tests
// =============================================================================

TEST(test_evio_once_by_poll)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    int fds[2];
    assert_equal(pipe(fds), 0);

    evio_once once;
    evio_once_init(&once, generic_cb, fds[0], EVIO_READ);
    evio_once_start(loop, &once, EVIO_TIME_FROM_SEC(10)); // Long timeout

    assert_equal(write(fds[1], "x", 1), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_ONCE);
    assert_true(generic_cb_emask & EVIO_READ);
    assert_equal(evio_refcount(loop), 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_once_by_timer)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    int fds[2];
    assert_equal(pipe(fds), 0);

    evio_once once;
    evio_once_init(&once, generic_cb, fds[0], EVIO_READ);
    evio_once_start(loop, &once, 0); // Immediate timeout

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_ONCE);
    assert_true(generic_cb_emask & EVIO_TIMER);
    assert_equal(evio_refcount(loop), 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

// =============================================================================
// Signal Tests
// =============================================================================

TEST(test_evio_signal)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    evio_signal sig;
    evio_signal_init(&sig, generic_cb, SIGUSR1);
    evio_signal_start(loop, &sig);
    assert_equal(evio_refcount(loop), 1);

    // Simulate signal delivery
    evio_feed_signal(loop, SIGUSR1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_equal(generic_cb_emask, EVIO_SIGNAL);

    evio_signal_stop(loop, &sig);
    assert_equal(evio_refcount(loop), 0);
    evio_loop_free(loop);
}

TEST(test_evio_signal_multiple_loops)
{
    evio_abort_cb old_abort = evio_get_abort();
    evio_set_abort(custom_abort_handler);
    custom_abort_called = 0;

    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    evio_signal sig1, sig2;
    evio_signal_init(&sig1, generic_cb, SIGUSR2);
    evio_signal_init(&sig2, generic_cb, SIGUSR2);

    evio_signal_start(loop1, &sig1);

    if (setjmp(abort_jmp_buf) == 0) {
        // This should abort because SIGUSR2 is already bound to loop1
        evio_signal_start(loop2, &sig2);
        assert_true(0); // Should not be reached
    }

    assert_equal(custom_abort_called, 1);

    // Cleanup
    evio_loop_free(loop1); // This will call evio_signal_cleanup_loop
    evio_loop_free(loop2);

    evio_set_abort(old_abort);
}

TEST(test_evio_signal_wrong_loop)
{
    reset_cb_state();
    evio_loop *loop1 = evio_loop_new(EVIO_FLAG_NONE);
    evio_loop *loop2 = evio_loop_new(EVIO_FLAG_NONE);
    evio_signal sig;

    evio_signal_init(&sig, generic_cb, SIGUSR1);
    evio_signal_start(loop1, &sig);

    // Feed signal to the wrong loop, should do nothing.
    evio_feed_signal(loop2, SIGUSR1);
    evio_run(loop2, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 0);

    evio_loop_free(loop1);
    evio_loop_free(loop2);
}

TEST(test_evio_signal_invalid_signum)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_not_null(loop);

    // feed invalid signal, should be a no-op and not crash
    evio_feed_signal(loop, -1);
    evio_feed_signal(loop, 0);
    evio_feed_signal(loop, NSIG);
    evio_feed_signal(loop, NSIG + 1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 0);

    evio_loop_free(loop);
}

// =============================================================================
// Other Watcher Smoke Tests
// =============================================================================

TEST(test_evio_prepare)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_prepare prepare;
    evio_prepare_init(&prepare, generic_cb);
    evio_prepare_start(loop, &prepare);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_equal(generic_cb_emask, EVIO_PREPARE);
    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

TEST(test_evio_check)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_check check;
    evio_check_init(&check, generic_cb);
    evio_check_start(loop, &check);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_equal(generic_cb_emask, EVIO_CHECK);
    evio_check_stop(loop, &check);
    evio_loop_free(loop);
}

TEST(test_evio_idle)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_idle idle;
    evio_idle_init(&idle, generic_cb);
    evio_idle_start(loop, &idle);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_equal(generic_cb_called, 1);
    assert_equal(generic_cb_emask, EVIO_IDLE);
    evio_idle_stop(loop, &idle);
    evio_loop_free(loop);
}

TEST(test_evio_cleanup)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_cleanup cleanup;
    evio_cleanup_init(&cleanup, generic_cb);
    evio_cleanup_start(loop, &cleanup);
    assert_equal(generic_cb_called, 0);
    evio_loop_free(loop);
    assert_equal(generic_cb_called, 1);
    assert_equal(generic_cb_emask, EVIO_CLEANUP);
}

// =============================================================================
// Main Test Runner
// =============================================================================

int main(void)
{
    struct {
        const char *name;
        void (*func)(const char *__unit);
    } tests[] = {
        // Allocation
        { "evio_malloc", test_evio_malloc },
        { "evio_calloc", test_evio_calloc },
        { "evio_realloc", test_evio_realloc },
        { "evio_reallocarray", test_evio_reallocarray },
        { "evio_custom_allocator", test_evio_custom_allocator },
        { "evio_alloc_overflow", test_evio_alloc_overflow },
        // Loop and Utility
        { "evio_ref_unref", test_evio_ref_unref },
        { "evio_userdata", test_evio_userdata },
        { "evio_loop_uring", test_evio_loop_uring },
        { "evio_break", test_evio_break },
        { "evio_clear_pending", test_evio_clear_pending },
        { "evio_strerror_valid", test_evio_strerror_valid },
        { "evio_strerror_invalid", test_evio_strerror_invalid },
        { "evio_strerror_erange", test_evio_strerror_erange },
        { "evio_utils_abort_custom", test_evio_utils_abort_custom },
        { "evio_core_requeue", test_evio_core_requeue },
        { "evio_poll_resizing_and_invalidation", test_evio_poll_resizing_and_invalidation },
        // Timer
        { "evio_timer", test_evio_timer },
        { "evio_timer_repeat", test_evio_timer_repeat },
        { "evio_timer_again_and_remaining", test_evio_timer_again_and_remaining },
        { "evio_timer_many", test_evio_timer_many },
        { "evio_timer_fast_repeat", test_evio_timer_fast_repeat },
        // Poll
        { "evio_poll", test_evio_poll },
        { "evio_poll_change", test_evio_poll_change },
        { "evio_poll_change_stop", test_evio_poll_change_stop },
        { "evio_feed_fd", test_evio_feed_fd },
        { "evio_poll_many_events", test_evio_poll_many_events },
        { "evio_poll_uring", test_evio_poll_uring },
        { "evio_poll_uring_eperm_error", test_evio_poll_uring_eperm_error },
        { "evio_poll_uring_eexist_error", test_evio_poll_uring_eexist_error },
        { "evio_poll_uring_enoent_error", test_evio_poll_uring_enoent_error },
        { "evio_poll_uring_many_events", test_evio_poll_uring_many_events },
        { "evio_poll_eperm_error", test_evio_poll_eperm_error },
        { "evio_poll_eexist_error", test_evio_poll_eexist_error },
        // EventFD
        { "evio_eventfd_eagain", test_evio_eventfd_eagain },
        // Async
        { "evio_async", test_evio_async },
        { "evio_async_pending", test_evio_async_pending },
        // Once
        { "evio_once_by_poll", test_evio_once_by_poll },
        { "evio_once_by_timer", test_evio_once_by_timer },
        // Signal
        { "evio_signal", test_evio_signal },
        { "evio_signal_multiple_loops", test_evio_signal_multiple_loops },
        { "evio_signal_wrong_loop", test_evio_signal_wrong_loop },
        { "evio_signal_invalid_signum", test_evio_signal_invalid_signum },
        // Other watchers
        { "evio_prepare", test_evio_prepare },
        { "evio_check", test_evio_check },
        { "evio_idle", test_evio_idle },
        { "evio_cleanup", test_evio_cleanup },
    };

    for (size_t i = 0, n = sizeof(tests) / sizeof(tests[0]); i < n; ++i) {
        fprintf(stdout, ">>> Testing (%zu of %zu) %s...\n", i + 1, n, tests[i].name);
        fflush(stdout);

        tests[i].func(tests[i].name);
    }

    fprintf(stdout, ">>> All tests passed!\n");
    return EXIT_SUCCESS;
}
