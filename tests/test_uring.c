#include "test.h"

#include "evio_uring.h"
#include "evio_uring_sys.h"

static struct {
    int fd;
    int op;
    int res;
    bool active;
} evio_uring_injection;

static struct {
    bool disable_single_mmap;

    unsigned mmap_call;
    unsigned mmap_fail_at;
    int mmap_err;

    bool setup_fail;
    int setup_err;

    bool register_fail;
    int register_err;

    bool epoll_fail;
    int epoll_err;

    bool eventfd_fail;
    int eventfd_err;

    bool force_sq_off_array_zero;

    bool enter_ret_active;
    int enter_ret;
    int enter_err;

    bool force_cq_empty;

    bool disable_register_probe;

    bool cqe_res_active;
    int cqe_res;
} evio_uring_probe_injection;

void evio_uring_test_inject_cqe_res_once(int fd, int op, int res)
{
    evio_uring_injection.fd = fd;
    evio_uring_injection.op = op;
    evio_uring_injection.res = res;
    evio_uring_injection.active = true;
}

void evio_uring_test_inject_reset(void)
{
    evio_uring_injection.active = false;
}

void evio_uring_test_probe_reset(void)
{
    memset(&evio_uring_probe_injection, 0, sizeof(evio_uring_probe_injection));
}

void evio_uring_test_probe_disable_single_mmap(bool disable)
{
    evio_uring_probe_injection.disable_single_mmap = disable;
}

void evio_uring_test_probe_fail_mmap_at(unsigned call, int err)
{
    evio_uring_probe_injection.mmap_fail_at = call;
    evio_uring_probe_injection.mmap_err = err;
}

void evio_uring_test_probe_fail_setup_once(int err)
{
    evio_uring_probe_injection.setup_fail = true;
    evio_uring_probe_injection.setup_err = err;
}

void evio_uring_test_probe_fail_register_once(int err)
{
    evio_uring_probe_injection.register_fail = true;
    evio_uring_probe_injection.register_err = err;
}

void evio_uring_test_probe_fail_epoll_create_once(int err)
{
    evio_uring_probe_injection.epoll_fail = true;
    evio_uring_probe_injection.epoll_err = err;
}

void evio_uring_test_probe_fail_eventfd_once(int err)
{
    evio_uring_probe_injection.eventfd_fail = true;
    evio_uring_probe_injection.eventfd_err = err;
}

void evio_uring_test_probe_force_sq_off_array_zero(bool force)
{
    evio_uring_probe_injection.force_sq_off_array_zero = force;
}

void evio_uring_test_probe_enter_ret_once(int ret, int err)
{
    evio_uring_probe_injection.enter_ret_active = true;
    evio_uring_probe_injection.enter_ret = ret;
    evio_uring_probe_injection.enter_err = err;
}

void evio_uring_test_probe_force_cq_empty(bool force)
{
    evio_uring_probe_injection.force_cq_empty = force;
}

void evio_uring_test_probe_disable_register_probe(bool disable)
{
    evio_uring_probe_injection.disable_register_probe = disable;
}

void evio_uring_test_probe_force_cqe_res_once(int res)
{
    evio_uring_probe_injection.cqe_res_active = true;
    evio_uring_probe_injection.cqe_res = res;
}

int evio_test_uring_setup(unsigned int entries, struct io_uring_params *params)
{
    if (evio_uring_probe_injection.setup_fail) {
        evio_uring_probe_injection.setup_fail = false;
        if (evio_uring_probe_injection.setup_err < 0) {
            return evio_uring_probe_injection.setup_err;
        }

        errno = evio_uring_probe_injection.setup_err ?
                evio_uring_probe_injection.setup_err : EPERM;
        return -1;
    }

    evio_uring_probe_injection.mmap_call = 0;
    return (int)syscall(SYS_io_uring_setup, entries, params);
}

int evio_test_uring_enter(unsigned int fd,
                          unsigned int to_submit,
                          unsigned int min_complete,
                          unsigned int flags,
                          sigset_t *sig, size_t sz)
{
    if (evio_uring_probe_injection.enter_ret_active) {
        evio_uring_probe_injection.enter_ret_active = false;
        if (evio_uring_probe_injection.enter_ret == -1) {
            errno = evio_uring_probe_injection.enter_err ?
                    evio_uring_probe_injection.enter_err : EINVAL;
        }
        return evio_uring_probe_injection.enter_ret;
    }

    return (int)syscall(SYS_io_uring_enter, fd, to_submit, min_complete, flags, sig, sz);
}

int evio_test_uring_register(unsigned int fd, unsigned int opcode,
                             const void *arg, unsigned int nr_args)
{
    if (evio_uring_probe_injection.register_fail) {
        evio_uring_probe_injection.register_fail = false;
        errno = evio_uring_probe_injection.register_err ?
                evio_uring_probe_injection.register_err : EPERM;
        return -1;
    }

    return (int)syscall(SYS_io_uring_register, fd, opcode, arg, nr_args);
}

void *evio_test_uring_mmap(void *addr, size_t length, int prot,
                           int flags, int fd, off_t offset)
{
    unsigned call = ++evio_uring_probe_injection.mmap_call;
    if (evio_uring_probe_injection.mmap_fail_at &&
        call == evio_uring_probe_injection.mmap_fail_at) {
        evio_uring_probe_injection.mmap_fail_at = 0;
        errno = evio_uring_probe_injection.mmap_err ?
                evio_uring_probe_injection.mmap_err : ENOMEM;
        return MAP_FAILED;
    }

    return mmap(addr, length, prot, flags, fd, offset);
}

int evio_test_uring_epoll_create1(int flags)
{
    if (evio_uring_probe_injection.epoll_fail) {
        evio_uring_probe_injection.epoll_fail = false;
        errno = evio_uring_probe_injection.epoll_err ?
                evio_uring_probe_injection.epoll_err : EMFILE;
        return -1;
    }

    return epoll_create1(flags);
}

int evio_test_uring_eventfd(unsigned int initval, int flags)
{
    if (evio_uring_probe_injection.eventfd_fail) {
        evio_uring_probe_injection.eventfd_fail = false;
        errno = evio_uring_probe_injection.eventfd_err ?
                evio_uring_probe_injection.eventfd_err : EMFILE;
        return -1;
    }

    return eventfd(initval, flags);
}

void evio_test_uring_probe_tweak_params(struct io_uring_params *params)
{
#ifdef IORING_SETUP_NO_SQARRAY
    if (evio_uring_probe_injection.force_sq_off_array_zero) {
        params->flags |= IORING_SETUP_NO_SQARRAY;
    }
#endif

#ifdef IORING_FEAT_SINGLE_MMAP
    if (evio_uring_probe_injection.disable_single_mmap) {
        params->features &= ~IORING_FEAT_SINGLE_MMAP;
    }
#endif
}

void evio_test_uring_probe_tweak_cq_tail(uint32_t *tail, uint32_t head)
{
    if (evio_uring_probe_injection.force_cq_empty) {
        *tail = head;
    }
}

void evio_test_uring_probe_tweak_cqe_res(int *res)
{
    if (evio_uring_probe_injection.cqe_res_active) {
        evio_uring_probe_injection.cqe_res_active = false;
        *res = evio_uring_probe_injection.cqe_res;
    }
}

bool evio_test_uring_cqe_res_override(int fd, int op, int *res)
{
    if (evio_uring_injection.active &&
        evio_uring_injection.fd == fd &&
        evio_uring_injection.op == op) {
        *res = evio_uring_injection.res;
        evio_uring_injection.active = false;
        return true;
    }

    return false;
}

bool evio_test_uring_probe_disable_register_probe(void)
{
    return evio_uring_probe_injection.disable_register_probe;
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

// A callback that reads from the fd to clear the event state.
static void read_and_count_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    char buf[1];
    evio_poll *io = container_of(base, evio_poll, base);
    read(io->fd, buf, sizeof(buf));
    generic_cb(loop, base, emask);
}

TEST(test_evio_loop_uring)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // Just run it once to make sure nothing crashes.
    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds1[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1), 0);

    int fds2[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds1[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    // Trigger on first pipe
    assert_int_equal(write(fds1[1], "a", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    data.called = 0;

    // Change to watch for write on the same fd
    char buf[1];
    assert_int_equal(read(fds1[0], buf, 1), 1); // clear pipe
    evio_poll_change(loop, &io, fds1[0], EVIO_WRITE);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    data.called = 0;

    // Change to watch for read on the second pipe
    evio_poll_change(loop, &io, fds2[0], EVIO_READ);
    assert_int_equal(write(fds2[1], "c", 1), 1); // write to new pipe
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);

    evio_poll_stop(loop, &io);
    close(fds1[0]);
    close(fds1[1]);
    close(fds2[0]);
    close(fds2[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_eperm_error)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fd = open("/dev/null", O_RDWR);
    assert_true(fd >= 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fd, EVIO_READ | EVIO_WRITE);
    io.data = &data;
    evio_poll_start(loop, &io);

    // The first run will submit an io_uring epoll_ctl, which will fail with EPERM.
    // Queue fd error.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // The callback was invoked with a poll event because of the error.
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_POLL);
    assert_true(data.emask & EVIO_READ);
    assert_true(data.emask & EVIO_WRITE);
    assert_false(data.emask & EVIO_ERROR);

    assert_true(io.active);
    assert_int_equal(evio_refcount(loop), 1);

    evio_poll_stop(loop, &io);
    close(fd);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_eexist_error)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    // Manually add the fd to the epoll set.
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_ADD, fds[0], &ev), 0);

    // Now, have evio start a watcher. It will try to ADD via uring, fail with EEXIST,
    // retry via MOD.
    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process the change

    // The watcher is active and working.
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_spurious_event)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    // Force extra EPOLLOUT in kernel mask.
    struct epoll_event ev = { .events = EPOLLIN | EPOLLOUT };
    ev.data.u64 = ((uint64_t)fds[0]) | ((uint64_t)loop->fds.ptr[fds[0]].gen << 32);
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_MOD, fds[0], &ev), 0);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 0);

    assert_int_equal(loop->fds.ptr[fds[0]].emask, EVIO_READ);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_enoent_error)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Add fd via uring

    // Manually remove the fd from epoll to create an inconsistent state.
    struct epoll_event ev = { 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_DEL, fds[0], &ev), 0);

    // Now, change the watcher. It will try to MOD, fail with ENOENT,
    // retry via ADD.
    evio_poll_change(loop, &io, fds[0], EVIO_READ | EVIO_WRITE);
    // Process change and recover from ENOENT via
    // re-adding the fd, and then receive the write-ready event.
    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_WRITE);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_autoflush)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    evio_poll io[EVIO_URING_EVENTS];
    int fds[EVIO_URING_EVENTS][2];

    // This loop triggers the auto-flush logic in evio_uring_ctl
    // when the submission queue becomes full.
    for (size_t i = 0; i < EVIO_URING_EVENTS; ++i) {
        fds[i][0] = fds[i][1] = -1;
        assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]), 0);
        evio_poll_init(&io[i], generic_cb, fds[i][0], EVIO_READ);
        io[i].data = &data;
        evio_poll_start(loop, &io[i]);
    }

    // After this, all watchers are active.
    assert_int_equal(evio_refcount(loop), EVIO_URING_EVENTS);

    // Trigger one event to check that things are working.
    assert_int_equal(write(fds[0][1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);

    // Cleanup
    for (size_t i = 0; i < EVIO_URING_EVENTS; ++i) {
        evio_poll_stop(loop, &io[i]);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    evio_loop_free(loop);
}

static size_t drain_uring_events_loop(evio_loop *loop, generic_cb_data *data,
                                      size_t expected_calls, size_t loop_limit)
{
    size_t loops = 0;
    while (data->called < expected_calls && loops < loop_limit) {
        evio_run(loop, EVIO_RUN_NOWAIT);
        loops++;
    }
    return loops;
}

TEST(test_evio_poll_uring_many_events)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    evio_poll io[EVIO_URING_EVENTS];
    int fds[EVIO_URING_EVENTS][2];

    for (size_t i = 0; i < EVIO_URING_EVENTS; ++i) {
        fds[i][0] = fds[i][1] = -1;
        assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]), 0);
        evio_poll_init(&io[i], read_and_count_cb, fds[i][0], EVIO_READ);
        io[i].data = &data;
        evio_poll_start(loop, &io[i]);
    }

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    // Write to all sockets to make them readable.
    for (size_t i = 0; i < EVIO_URING_EVENTS; ++i) {
        assert_int_equal(write(fds[i][1], "x", 1), 1);
    }

    // Drain all pending events from the loop.
    drain_uring_events_loop(loop, &data, EVIO_URING_EVENTS, EVIO_URING_EVENTS * 2);
    assert_int_equal(data.called, EVIO_URING_EVENTS);

    // Cleanup
    for (size_t i = 0; i < EVIO_URING_EVENTS; ++i) {
        evio_poll_stop(loop, &io[i]);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_many_events_loop_break)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);
    evio_poll io;

    evio_poll_init(&io, read_and_count_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    // No events; loop times out.
    size_t loops = drain_uring_events_loop(loop, &data, 1, 5);
    assert_int_equal(loops, 5);
    assert_int_equal(data.called, 0);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_ebadf_error)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process ADD via uring

    // Close the fd, then queue a change.
    close(fds[0]);
    evio_poll_change(loop, &io, fds[0], EVIO_READ | EVIO_WRITE);

    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_invoke_pending(loop);

    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_ERROR);
    assert_false(io.active);

    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_uring_gen_rollback_on_eexist)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    // Ensure the fds array is large enough to access fds[0]
    // by creating and immediately stopping a watcher on that fd
    {
        evio_poll tmp_io;
        evio_poll_init(&tmp_io, generic_cb, fds[0], EVIO_READ);
        evio_poll_start(loop, &tmp_io);
        evio_poll_stop(loop, &tmp_io);
    }

    // Manually add the fd to the epoll set to trigger EEXIST
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_ADD, fds[0], &ev), 0);

    uint32_t gen_before = loop->fds.ptr[fds[0]].gen;

    evio_uring_test_inject_cqe_res_once(fds[0], EPOLL_CTL_ADD, -EEXIST);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(loop->fds.ptr[fds[0]].gen, gen_before + 1);

    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_uring_gen_rollback_on_enoent)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT);

    uint32_t gen_before = loop->fds.ptr[fds[0]].gen;

    struct epoll_event ev = { 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_DEL, fds[0], &ev), 0);

    evio_uring_test_inject_cqe_res_once(fds[0], EPOLL_CTL_MOD, -ENOENT);

    evio_poll_change(loop, &io, fds[0], EVIO_READ | EVIO_WRITE);
    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(loop->fds.ptr[fds[0]].gen, gen_before + 1);

    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_uring_no_retry_on_wrong_op)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    // Add the fd normally first
    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT);

    uint32_t gen_before = loop->fds.ptr[fds[0]].gen;

    evio_uring_test_inject_cqe_res_once(fds[0], EPOLL_CTL_MOD, -EEXIST);

    // Change the watcher
    evio_poll_change(loop, &io, fds[0], EVIO_WRITE);

    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_invoke_pending(loop);

    assert_int_equal(loop->fds.ptr[fds[0]].gen, gen_before);
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_ERROR);
    assert_false(io.active);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_uring_no_retry_on_enoent_add)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_uring_test_inject_cqe_res_once(fds[0], EPOLL_CTL_ADD, -ENOENT);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    uint32_t gen_before = loop->fds.ptr[fds[0]].gen;
    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_invoke_pending(loop);

    assert_int_equal(loop->fds.ptr[fds[0]].gen, gen_before);
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_ERROR);
    assert_false(io.active);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_uring_no_retry_on_ebadf_injected)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    // First add the fd normally
    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT);

    uint32_t gen_before = loop->fds.ptr[fds[0]].gen;

    // Inject EBADF for a MOD operation - this triggers the default case
    evio_uring_test_inject_cqe_res_once(fds[0], EPOLL_CTL_MOD, -EBADF);

    // Change the watcher to trigger a MOD
    evio_poll_change(loop, &io, fds[0], EVIO_READ | EVIO_WRITE);
    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_invoke_pending(loop);

    // EBADF triggers evio_queue_fd_errors which stops the watcher
    // and queues an error event
    assert_int_equal(loop->fds.ptr[fds[0]].gen, gen_before);
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_ERROR);
    assert_false(io.active);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_uring_probe_epoll_ctl)
{
    // EVIO_FLAG_URING probe smoke-test.
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        print_message("io_uring: unsupported\n");
    } else {
        print_message("io_uring: supported\n");
    }
    // GCOVR_EXCL_STOP

    evio_loop_free(loop);
}

static bool evio_test_uring_supported(void)
{
    evio_uring_test_probe_reset();
    evio_uring *iou = evio_uring_new();
    if (!iou) {
        return false;
    }
    evio_uring_free(iou);
    return true;
}

TEST(test_evio_uring_probe_fail_single_mmap)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(false);
    evio_uring_test_probe_fail_mmap_at(1, ENOMEM);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_single_mmap_default_errno)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(false);
    evio_uring_test_probe_fail_mmap_at(1, 0);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_sq_mmap)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(true);
    evio_uring_test_probe_fail_mmap_at(1, ENOMEM);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_cq_mmap)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(true);
    evio_uring_test_probe_fail_mmap_at(2, ENOMEM);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_sqe_mmap)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(true);
    evio_uring_test_probe_fail_mmap_at(3, ENOMEM);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_sqe_mmap_single_mmap)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(false);
    evio_uring_test_probe_fail_mmap_at(2, ENOMEM);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_epoll_create)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(true);
    evio_uring_test_probe_fail_epoll_create_once(EMFILE);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_epoll_create_single_mmap)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(false);
    evio_uring_test_probe_fail_epoll_create_once(EMFILE);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_epoll_create_default_errno)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(true);
    evio_uring_test_probe_fail_epoll_create_once(0);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_eventfd)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(true);
    evio_uring_test_probe_fail_eventfd_once(EMFILE);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_eventfd_single_mmap)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(false);
    evio_uring_test_probe_fail_eventfd_once(EMFILE);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_eventfd_default_errno)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(true);
    evio_uring_test_probe_fail_eventfd_once(0);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_setup_default_errno)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_fail_setup_once(0);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_setup_errno)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_fail_setup_once(EPERM);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_unsupported_enosys)
{
    evio_uring_test_probe_reset();
    evio_uring_test_probe_fail_setup_once(ENOSYS);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_unsupported_enosys_negative)
{
    evio_uring_test_probe_reset();
    evio_uring_test_probe_fail_setup_once(-ENOSYS);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_setup_fail_branches)
{
    evio_uring_test_probe_reset();
    evio_uring_test_probe_fail_setup_once(EPERM);
    assert_null(evio_uring_new());

    evio_uring_test_probe_reset();
    evio_uring_test_probe_fail_setup_once(ENOSYS);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_enter_ret)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_enter_ret_once(0, 0);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_enter_errno_default)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_enter_ret_once(-1, 0);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fail_enter_errno)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_enter_ret_once(-1, EINTR);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_empty_cq)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_enter_ret_once(1, 0);
    evio_uring_test_probe_force_cq_empty(true);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_fallback_without_register_probe)
{
#ifndef IORING_REGISTER_PROBE
    TEST_SKIP();
#else
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_register_probe(true);

    evio_uring *iou = evio_uring_new();
    assert_non_null(iou);
    evio_uring_free(iou);
#endif
}

TEST(test_evio_uring_probe_fallback_on_register_fail)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_fail_register_once(EPERM);

    evio_uring *iou = evio_uring_new();
    assert_non_null(iou);
    evio_uring_free(iou);
}

TEST(test_evio_uring_test_register_wrapper)
{
    evio_uring_test_probe_reset();
    evio_uring_test_probe_fail_register_once(EPERM);
    assert_int_equal(evio_test_uring_register(0, 0, NULL, 0), -1);

    (void)evio_test_uring_register(0, 0, NULL, 0);
}

TEST(test_evio_uring_test_disable_register_probe_flag)
{
    evio_uring_test_probe_reset();
    assert_false(evio_test_uring_probe_disable_register_probe());

    evio_uring_test_probe_disable_register_probe(true);
    assert_true(evio_test_uring_probe_disable_register_probe());

    evio_uring_test_probe_disable_register_probe(false);
    assert_false(evio_test_uring_probe_disable_register_probe());
}

TEST(test_evio_uring_probe_force_einval)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_force_cqe_res_once(-EINVAL);
    assert_null(evio_uring_new());
}

TEST(test_evio_uring_probe_force_other_error)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_force_cqe_res_once(-EFAULT);
    assert_null(evio_uring_new());
}

#ifdef IORING_SETUP_NO_SQARRAY
TEST(test_evio_uring_probe_no_sqarray)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_force_sq_off_array_zero(true);

    evio_uring *iou = evio_uring_new();
    assert_non_null(iou);
    evio_uring_free(iou);
}
#endif

TEST(test_evio_uring_inject_mismatch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

    // GCOVR_EXCL_START
    if (!loop->iou) {
        evio_loop_free(loop);
        TEST_SKIPF("io_uring unsupported by kernel");
    }
    // GCOVR_EXCL_STOP

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    evio_uring_test_inject_cqe_res_once(1234567, EPOLL_CTL_ADD, -EEXIST);
    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_uring_test_inject_reset();

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_uring_probe_munmap_cq)
{
    if (!evio_test_uring_supported()) {
        TEST_SKIPF("io_uring unsupported by kernel"); // GCOVR_EXCL_LINE
    }

    evio_uring_test_probe_reset();
    evio_uring_test_probe_disable_single_mmap(true);

    evio_uring *iou = evio_uring_new();
    if (iou) {
        evio_uring_free(iou);
    }
}
