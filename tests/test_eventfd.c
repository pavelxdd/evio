#include "test.h"

#include <sys/resource.h>

TEST(test_evio_eventfd_init_fail)
{
    struct rlimit old_lim;
    // GCOVR_EXCL_START
    if (getrlimit(RLIMIT_NOFILE, &old_lim) != 0) {
        print_message("      -> Skipping test, could not get rlimit\n");
        return;
    }
    // GCOVR_EXCL_STOP

    // Create loop first to get epoll fd allocated.
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

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
        evio_loop_free(loop);
        setrlimit(RLIMIT_NOFILE, &old_lim);
        return;
    }
    // GCOVR_EXCL_STOP

    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);
    evio_set_abort(custom_abort_handler, NULL);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        // This should fail because we've exhausted fds.
        evio_eventfd_init(loop);
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_set_abort(old_abort, old_abort_ctx);
    evio_loop_free(loop);
    setrlimit(RLIMIT_NOFILE, &old_lim);
}

TEST(test_evio_eventfd_eagain)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_eventfd_init(loop);
    int fd = loop->event.fd;
    assert_true(fd >= 0);

    // Set the eventfd counter to a value that will cause the next write(1) to fail.
    uint64_t val = UINT64_MAX - 1;
    assert_int_equal(write(fd, &val, sizeof(val)), sizeof(val));

    // Manually allow eventfd writes to simulate the state inside evio_run
    atomic_store_explicit(&loop->eventfd_allow.value, 1, memory_order_relaxed);

    // This call to evio_eventfd_write will first attempt a write(1), which fails with EAGAIN.
    // It will then read() the counter (getting UINT64_MAX - 1 and resetting it to 0),
    // and then successfully write(1).
    evio_eventfd_write(loop);

    // The final value in the eventfd should be 1.
    uint64_t result_val;
    assert_int_equal(read(fd, &result_val, sizeof(result_val)), sizeof(result_val));
    assert_int_equal(result_val, 1);

    evio_loop_free(loop);
}

TEST(test_evio_eventfd_write_pending)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    evio_eventfd_init(loop);

    // First call: event_pending becomes 1, but returns due to eventfd_allow being 0.
    // The if() in evio_eventfd_write will be false.
    evio_eventfd_write(loop);
    assert_true(atomic_load_explicit(&loop->event_pending.value, memory_order_relaxed));

    // Second call: event_pending is already 1, so the if() is true and it returns.
    evio_eventfd_write(loop);

    // The eventfd counter should still be 0 as write was never successful.
    uint64_t val;
    ssize_t res = read(loop->event.fd, &val, sizeof(val));
    assert_int_equal(res, -1);
    assert_int_equal(errno, EAGAIN);

    evio_loop_free(loop);
}

TEST(test_evio_eventfd_cb_async_event)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    reset_cb_state();

    evio_async w1, w2;
    evio_async_init(&w1, generic_cb);
    evio_async_init(&w2, generic_cb2);
    evio_async_start(loop, &w1);
    evio_async_start(loop, &w2);

    // This will set async_pending and w->status, and wake up loop via eventfd.
    evio_async_send(loop, &w1);

    // Simulate loop calling the eventfd callback.
    // This will iterate over both async watchers.
    // For `w`, the status will be 1 -> event queued (if branch).
    // For `w2`, the status will be 0 -> no event queued (else branch).
    evio_eventfd_cb(loop, &loop->event.base, EVIO_READ);

    // The callback should have queued an async event for w1
    assert_int_equal(evio_pending_count(loop), 1);

    // Invoke pending events
    evio_invoke_pending(loop);

    assert_int_equal(generic_cb_called, 1);
    assert_int_equal(generic_cb_emask, EVIO_ASYNC);
    assert_int_equal(generic_cb2_called, 0);

    evio_async_stop(loop, &w1);
    evio_async_stop(loop, &w2);
    evio_loop_free(loop);
    reset_cb_state();
}

TEST(test_evio_eventfd_double_init)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // First init
    evio_eventfd_init(loop);
    assert_true(loop->event.active);
    int fd = loop->event.fd;
    assert_true(fd >= 0);

    // Second init should be a no-op
    evio_eventfd_init(loop);
    assert_int_equal(loop->event.fd, fd);

    evio_loop_free(loop);
}

TEST(test_evio_eventfd_write_not_allowed)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    evio_eventfd_init(loop);

    // eventfd_allow is 0 by default.
    // Try to write. It should return without doing anything.
    evio_eventfd_write(loop);

    // The eventfd counter should be 0.
    uint64_t val;
    ssize_t res = read(loop->event.fd, &val, sizeof(val));
    assert_int_equal(res, -1);
    assert_int_equal(errno, EAGAIN);

    evio_loop_free(loop);
}
