#include "test.h"
#include "abort.h"

#include <sys/resource.h>

#include "evio_eventfd.h"
#include "evio_eventfd_sys.h"

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

static struct {
    bool read_fail;
    int read_err;
    bool write_fail;
    int write_err;
} evio_eventfd_inject;

void evio_eventfd_test_inject_read_fail_once(int err)
{
    evio_eventfd_inject.read_fail = true;
    evio_eventfd_inject.read_err = err;
}

void evio_eventfd_test_inject_write_fail_once(int err)
{
    evio_eventfd_inject.write_fail = true;
    evio_eventfd_inject.write_err = err;
}

ssize_t evio_test_eventfd_read(int fd, void *buf, size_t count)
{
    if (evio_eventfd_inject.read_fail) {
        evio_eventfd_inject.read_fail = false;
        errno = evio_eventfd_inject.read_err ? evio_eventfd_inject.read_err : EINTR;
        return -1;
    }

    return read(fd, buf, count);
}

ssize_t evio_test_eventfd_write(int fd, const void *buf, size_t count)
{
    if (evio_eventfd_inject.write_fail) {
        evio_eventfd_inject.write_fail = false;
        errno = evio_eventfd_inject.write_err ? evio_eventfd_inject.write_err : EAGAIN;
        return -1;
    }

    return write(fd, buf, count);
}

TEST(test_evio_eventfd_init_fail)
{
    struct rlimit old_lim;
    // GCOVR_EXCL_START
    if (getrlimit(RLIMIT_NOFILE, &old_lim) != 0) {
        TEST_SKIPF("getrlimit");
    }
    // GCOVR_EXCL_STOP

    // Create loop first to get epoll fd allocated.
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Set rlimit to current fd count.
    int next_fd = dup(0);
    close(next_fd);

    struct rlimit new_lim;
    new_lim.rlim_cur = next_fd;
    new_lim.rlim_max = old_lim.rlim_max;

    // GCOVR_EXCL_START
    if (setrlimit(RLIMIT_NOFILE, &new_lim) != 0) {
        evio_loop_free(loop);
        setrlimit(RLIMIT_NOFILE, &old_lim);
        TEST_SKIPF("setrlimit");
    }
    // GCOVR_EXCL_STOP

    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };
    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    if (setjmp(jmp) == 0) {
        evio_eventfd_init(loop);
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(abort_ctx.called, 1);

    evio_test_abort_ctx_end(&abort_ctx);
    evio_loop_free(loop);
    setrlimit(RLIMIT_NOFILE, &old_lim);
}

TEST(test_evio_eventfd_drain_read_eintr)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_eventfd_init(loop);

    atomic_store_explicit(&loop->eventfd_allow.value, 1, memory_order_relaxed);

    evio_eventfd_test_inject_write_fail_once(EAGAIN);
    evio_eventfd_test_inject_read_fail_once(EINTR);

    evio_eventfd_write(loop);

    evio_loop_free(loop);
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

    uint64_t result_val;
    assert_int_equal(read(fd, &result_val, sizeof(result_val)), sizeof(result_val));
    assert_int_equal(result_val, 1);

    evio_loop_free(loop);
}

static FILE *eventfd_ebadf_abort_handler(void *ctx)
{
    evio_loop *loop = ctx;
    // Restore the event.fd to -1 so evio_loop_free doesn't try to close
    // the read-only fd we dup'd over it.
    loop->event.fd = -1;
    return NULL;
}

TEST(test_evio_eventfd_write_ebadf)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    evio_eventfd_init(loop);

    int event_fd = loop->event.fd;
    assert_true(event_fd >= 0);

    // Open a read-only fd to dup over the eventfd
    int read_only_fd = open("/dev/null", O_RDONLY);
    assert_true(read_only_fd >= 0);

    // Now event_fd refers to a read-only file.
    int ret = dup2(read_only_fd, event_fd);
    assert_int_equal(ret, event_fd);
    close(read_only_fd); // close the original /dev/null fd, we don't need it.

    // Allow eventfd writes to proceed to evio_eventfd_notify
    atomic_store_explicit(&loop->eventfd_allow.value, 1, memory_order_relaxed);

    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };
    evio_test_abort_ctx_begin(&abort_ctx, &jmp);
    abort_ctx.cb = eventfd_ebadf_abort_handler;
    abort_ctx.cb_ctx = loop;

    // EBADF => EVIO_ABORT.
    if (setjmp(jmp) == 0) {
        evio_eventfd_write(loop);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(abort_ctx.called, 1);

    // Restore abort state. The loop's event.fd was set to -1 by our handler.
    evio_test_abort_ctx_end(&abort_ctx);
    close(event_fd);
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

    uint64_t val;
    ssize_t res = read(loop->event.fd, &val, sizeof(val));
    assert_int_equal(res, -1);
    assert_int_equal(errno, EAGAIN);

    evio_loop_free(loop);
}

TEST(test_evio_eventfd_write_allowed_keeps_pending)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);
    evio_eventfd_init(loop);

    atomic_store_explicit(&loop->eventfd_allow.value, 1, memory_order_relaxed);

    evio_eventfd_write(loop);
    assert_true(atomic_load_explicit(&loop->event_pending.value, memory_order_relaxed));

    evio_eventfd_cb(loop, &loop->event.base, EVIO_READ);
    assert_false(atomic_load_explicit(&loop->event_pending.value, memory_order_relaxed));

    evio_loop_free(loop);
}

TEST(test_evio_eventfd_cb_async_event)
{
    generic_cb_data data1 = { 0 };
    generic_cb_data data2 = { 0 };

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_async async1;
    evio_async_init(&async1, generic_cb);
    async1.data = &data1;
    evio_async_start(loop, &async1);

    evio_async async2;
    evio_async_init(&async2, generic_cb);
    async2.data = &data2;
    evio_async_start(loop, &async2);

    evio_async_send(loop, &async1);

    // Simulate loop calling the eventfd callback.
    evio_eventfd_cb(loop, &loop->event.base, EVIO_READ);

    assert_int_equal(evio_pending_count(loop), 1);

    // Invoke pending events
    evio_invoke_pending(loop);

    assert_int_equal(data1.called, 1);
    assert_int_equal(data1.emask, EVIO_ASYNC);
    assert_int_equal(data2.called, 0);

    evio_async_stop(loop, &async1);
    evio_async_stop(loop, &async2);
    evio_loop_free(loop);
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

    // Second init: no-op
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
    evio_eventfd_write(loop);

    uint64_t val;
    ssize_t res = read(loop->event.fd, &val, sizeof(val));
    assert_int_equal(res, -1);
    assert_int_equal(errno, EAGAIN);

    evio_loop_free(loop);
}
