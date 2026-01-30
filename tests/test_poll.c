#include "test.h"
#include "abort.h"

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

// GCOVR_EXCL_START
static void dummy_cb(evio_loop *loop, evio_base *base, evio_mask emask) {}
// GCOVR_EXCL_STOP

TEST(test_evio_poll)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    // Double start: no-op
    evio_poll_start(loop, &io);
    assert_int_equal(evio_refcount(loop), 1);

    // Write to the pipe to trigger a read event
    assert_int_equal(write(fds[1], "x", 1), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_READ);

    evio_poll_stop(loop, &io);
    assert_int_equal(evio_refcount(loop), 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_update_force_poll_flag)
{
    generic_cb_data data1 = { 0 };
    generic_cb_data data2 = { 0 };

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io1;
    evio_poll_init(&io1, generic_cb, fds[0], EVIO_READ);
    io1.data = &data1;

    evio_poll io2;
    evio_poll_init(&io2, generic_cb, fds[0], EVIO_READ);
    io2.data = &data2;

    // Start one watcher and run the loop to establish a baseline.
    evio_poll_start(loop, &io1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(loop->fds.ptr[fds[0]].emask, EVIO_READ);

    // Start a second watcher on the same fd with the same mask.
    // This queues a change with the EVIO_POLL flag set.
    evio_poll_start(loop, &io2);
    assert_int_equal(loop->fdchanges.count, 1);

    // Run the loop. evio_poll_update will be called.
    // The new mask will equal the old mask, but the EVIO_POLL flag will be set,
    // causing the optimization to be skipped.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // Both watchers are active.
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data1.called, 1);
    assert_int_equal(data2.called, 1);

    evio_poll_stop(loop, &io1);
    evio_poll_stop(loop, &io2);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_update_assert_fd_bounds)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, dummy_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io); // This queues a change for fds[0]
    assert_int_equal(loop->fdchanges.count, 1);

    int original_fd = loop->fdchanges.ptr[0];
    loop->fdchanges.ptr[0] = -1;
    expect_assert_failure(evio_poll_update(loop));

    // Restore for next test. After longjmp, fdchanges.count is still 1.
    loop->fdchanges.ptr[0] = original_fd;

    loop->fdchanges.ptr[0] = loop->fds.count;
    expect_assert_failure(evio_poll_update(loop));

    // Restore and cleanup
    loop->fdchanges.ptr[0] = original_fd;
    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_update_assert_backpointer)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, dummy_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io); // This queues a change for fds[0]

    assert_int_equal(loop->fdchanges.count, 1);
    assert_int_equal(loop->fds.ptr[io.fd].changes, 1);

    // Corrupt backpointer
    loop->fds.ptr[io.fd].changes = 99;
    expect_assert_failure(evio_poll_update(loop));

    // Restore and cleanup
    loop->fds.ptr[io.fd].changes = 1;
    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_change)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds1[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1), 0);

    int fds2[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2), 0);

    evio_poll io;
    // Use a callback that consumes the read event to avoid level-triggering issues.
    evio_poll_init(&io, read_and_count_cb, fds1[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    // Trigger on first pipe
    assert_int_equal(write(fds1[1], "a", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    data.called = 0;
    data.emask = 0;

    // Change to watch for write on the same fd.
    // Switch to a non-reading callback for this part.
    evio_poll_change(loop, &io, fds1[0], EVIO_WRITE);
    io.base.cb = generic_cb;
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_WRITE);
    data.called = 0;

    // Change to watch for read on the second pipe.
    // Switch back to the reading callback.
    evio_poll_change(loop, &io, fds2[0], EVIO_READ);
    io.base.cb = read_and_count_cb;

    assert_int_equal(write(fds1[1], "b", 1), 1); // write to old pipe
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0); // does not trigger

    assert_int_equal(write(fds2[1], "c", 1), 1); // write to new pipe
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    data.called = 0;

    // Change with same mask no-op. Since the data was read,
    // fd is no longer readable, so no event fires.
    evio_poll_change(loop, &io, fds2[0], EVIO_READ);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    // Change to a new fd with emask=0. This stops the watcher.
    int fds3[2] = { -1, -1 };
    assert_int_equal(pipe(fds3), 0);
    evio_poll_change(loop, &io, fds3[0], 0);
    assert_false(io.active);
    assert_int_equal(evio_refcount(loop), 0);
    close(fds3[0]);
    close(fds3[1]);

    evio_poll_stop(loop, &io);
    close(fds1[0]);
    close(fds1[1]);
    close(fds2[0]);
    close(fds2[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_change_stop)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);
    assert_int_equal(evio_refcount(loop), 1);

    // Change with emask=0 stops the watcher.
    evio_poll_change(loop, &io, fds[0], 0);
    assert_int_equal(evio_refcount(loop), 0);
    assert_false(io.active);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_modify)
{
    evio_poll w;
    // Initial state to ensure EVIO_POLL is present
    evio_poll_init(&w, dummy_cb, 0, 0);
    w.emask = EVIO_READ | EVIO_POLL;

    // Modify to WRITE; preserve POLL.
    evio_poll_modify(&w, EVIO_WRITE);
    assert_int_equal(w.emask, EVIO_WRITE | EVIO_POLL);

    // Modify with extra flags; keep READ/WRITE only.
    evio_poll_modify(&w, EVIO_READ | EVIO_TIMER);
    assert_int_equal(w.emask, EVIO_READ | EVIO_POLL);

    // Modify to 0; preserve POLL.
    evio_poll_modify(&w, 0);
    assert_int_equal(w.emask, EVIO_POLL);
}

typedef struct {
    pthread_t thread;
    int fd;
} eintr_thread_arg;

static void *eintr_thread_func(void *ptr)
{
    eintr_thread_arg *arg = ptr;
    // Let main thread enter epoll_pwait
    usleep(50000); // 50ms
    pthread_kill(arg->thread, SIGUSR1);
    // After interrupting, write to the pipe to unblock the second epoll_pwait call
    usleep(10000); // give it a moment to re-enter epoll_pwait
    assert_int_equal(write(arg->fd, "x", 1), 1);
    return NULL;
}

// Do nothing, just to interrupt syscall
static void sigusr1_handler(int signum) {}

TEST(test_evio_poll_wait_eintr)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Setup a poll watcher on a pipe to make the loop block.
    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    // Setup a dummy signal handler for SIGUSR1
    struct sigaction sa_new = { .sa_handler = sigusr1_handler };
    struct sigaction sa_old;
    assert_int_equal(sigaction(SIGUSR1, &sa_new, &sa_old), 0);

    pthread_t thread;
    eintr_thread_arg arg = { .thread = pthread_self(), .fd = fds[1] };
    assert_int_equal(pthread_create(&thread, NULL, eintr_thread_func, &arg), 0);

    // EINTR retry path.
    evio_run(loop, EVIO_RUN_ONCE);

    assert_int_equal(pthread_join(thread, NULL), 0);

    // The poll watcher's callback was called.
    assert_int_equal(data.called, 1);

    // Restore default handler
    assert_int_equal(sigaction(SIGUSR1, &sa_old, NULL), 0);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_feed_fd)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    // Feed an event manually for the fd.
    evio_feed_fd_event(loop, fds[0], EVIO_READ);

    // Run and check that the callback was called.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_READ);

    data.called = 0;
    data.emask = 0;

    // Feed an error manually for the fd.
    evio_feed_fd_error(loop, fds[0]);

    // Stops watcher; callback gets EVIO_ERROR.
    evio_invoke_pending(loop);
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_ERROR);
    assert_false(io.active);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_update_empty_emask)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    // emask=0 (EVIO_POLL set internally).
    evio_poll_init(&io, generic_cb, fds[0], 0);
    io.data = &data;

    // Start queues a change; emask stays 0.
    evio_poll_start(loop, &io);
    assert_int_equal(io.emask, 0);
    assert_int_equal(loop->fdchanges.count, 1);

    // Process the change with fds->emask == 0.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callback was called because nothing was watched.
    assert_int_equal(data.called, 0);
    // The change was processed.
    assert_int_equal(loop->fdchanges.count, 0);

    // The watcher is still active but idle.
    assert_true(io.active);

    // Now, change it to watch for an event to prove it's still usable.
    evio_poll_change(loop, &io, fds[0], EVIO_READ);
    assert_int_equal(io.emask, EVIO_READ);
    assert_int_equal(loop->fdchanges.count, 1);
    evio_run(loop, EVIO_RUN_NOWAIT); // process the change

    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_change_inactive)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    // Watcher is initialized but not started, so it's inactive.

    // change() on an inactive watcher starts it.
    evio_poll_change(loop, &io, fds[0], EVIO_READ);

    // Watcher is active.
    assert_true(io.active);
    assert_int_equal(evio_refcount(loop), 1);

    // Trigger an event to make sure it's working.
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

static size_t drain_events_loop(evio_loop *loop, generic_cb_data *data, size_t expected_calls,
                                size_t loop_limit)
{
    size_t loops = 0;
    while (data->called < expected_calls && loops < loop_limit) {
        evio_run(loop, EVIO_RUN_NOWAIT);
        loops++;
    }
    return loops;
}

#define MANY_FDS 100
TEST(test_evio_poll_many_events)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_poll io[MANY_FDS];
    int fds[MANY_FDS][2];

    for (int i = 0; i < MANY_FDS; ++i) {
        fds[i][0] = fds[i][1] = -1;
        assert_int_equal(pipe(fds[i]), 0);
        // Use the read_and_count_cb to consume the event
        evio_poll_init(&io[i], read_and_count_cb, fds[i][0], EVIO_READ);
        // Store the read-end of the pipe in the watcher's data field
        io[i].data = &data;
        evio_poll_start(loop, &io[i]);
    }

    // Run the loop once to ensure evio_poll_update() runs
    // and all fds are added to epoll. We don't expect events yet.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    // Write to all pipes to make them readable.
    for (int i = 0; i < MANY_FDS; ++i) {
        assert_int_equal(write(fds[i][1], "x", 1), 1);
    }

    // Drain all pending events from the loop.
    drain_events_loop(loop, &data, MANY_FDS, (size_t)MANY_FDS * 2);
    assert_int_equal(data.called, MANY_FDS);

    // Cleanup
    for (int i = 0; i < MANY_FDS; ++i) {
        evio_poll_stop(loop, &io[i]);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    evio_loop_free(loop);
}

TEST(test_evio_poll_many_events_loop_break)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, read_and_count_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    // No events; loop times out.
    size_t loops = drain_events_loop(loop, &data, 1, 5);
    assert_int_equal(loops, 5);
    assert_int_equal(data.called, 0);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_enoent_error)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Add fd to epoll

    // Manually remove the fd from epoll to create an inconsistent state.
    struct epoll_event ev = { 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_DEL, fds[0], &ev), 0);

    // Now, change the watcher. It will try to MOD, fail with ENOENT,
    // recover via ADD.
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

// EPERM path: fd can't be polled (e.g. regular file).
TEST(test_evio_poll_eperm_error)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fd = open("/dev/null", O_RDWR);
    assert_true(fd >= 0);

    evio_poll io;
    // Watch for both read and write, as /dev/null is always ready for both.
    evio_poll_init(&io, generic_cb, fd, EVIO_READ | EVIO_WRITE);
    io.data = &data;
    evio_poll_start(loop, &io);

    // The first run will call epoll_ctl, which will fail with EPERM.
    // Treat as permanently ready.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // The callback was invoked with a normal poll event.
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_POLL);
    assert_true(data.emask & EVIO_READ);
    assert_true(data.emask & EVIO_WRITE);
    assert_false(data.emask & EVIO_ERROR);

    // Watcher stays active.
    assert_true(io.active);
    assert_int_equal(evio_refcount(loop), 1);

    evio_poll_stop(loop, &io);
    close(fd);
    evio_loop_free(loop);
}

TEST(test_evio_poll_eexist_error)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    // Manually add the fd to the epoll set to create an inconsistent state.
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_ADD, fds[0], &ev), 0);

    // Now, have evio start a watcher. It will try to ADD, fail with EEXIST,
    // recover via MOD.
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

TEST(test_evio_poll_update_no_change)
{
    generic_cb_data data1 = { 0 };
    generic_cb_data data2 = { 0 };

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io1;
    evio_poll_init(&io1, generic_cb, fds[0], EVIO_READ);
    io1.data = &data1;

    evio_poll io2;
    evio_poll_init(&io2, generic_cb, fds[0], EVIO_READ);
    io2.data = &data2;

    // Start two watchers on the same fd.
    evio_poll_start(loop, &io1);
    evio_poll_start(loop, &io2);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process ADD.

    // Stop one watcher. This queues a change, but the resulting event mask
    // for the fd is the same (still EVIO_READ).
    evio_poll_stop(loop, &io1);

    // Run the loop. poll_update hits the
    // optimization branch and do nothing.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // Remaining watcher works.
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data1.called, 0); // io1's cb not called
    assert_int_equal(data2.called, 1); // io2's cb called

    evio_poll_stop(loop, &io2);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_eexist_same_mask)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;

    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(loop->fds.ptr[fds[0]].emask, EVIO_READ);

    evio_poll_stop(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(loop->fds.ptr[fds[0]].emask, 0);

    // Manually add fd back to epoll to create desync.
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_ADD, fds[0], &ev), 0);

    // Start watcher again. This queues a change.
    evio_poll_start(loop, &io);

    // Run. poll_update will:
    // - See old emask was 0, so op=ADD.
    // - epoll_ctl(ADD) fails with EEXIST.
    // - recovery calls epoll_ctl(MOD).
    evio_run(loop, EVIO_RUN_NOWAIT);

    // Watcher is working.
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_gen_counter)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    int fd = fds[0];

    evio_poll io;
    evio_poll_init(&io, generic_cb, fd, EVIO_READ);
    io.data = &data;

    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(loop->fds.ptr[fd].gen, 1);

    // Trigger a READ event by writing to the pipe.
    assert_int_equal(write(fds[1], "x", 1), 1);

    // Force a change by modifying the event mask. This queues a change.
    evio_poll_change(loop, &io, fd, EVIO_READ | EVIO_WRITE);

    // Run the loop.
    // - poll_update will call epoll_ctl(MOD), incrementing gen to 2.
    // - The fd is still readable from step 2, and is also writeable.
    // - epoll_wait returns a single event for gen=2 with events EPOLLIN | EPOLLOUT.
    // - The gen check passes, and the callback is invoked with both READ and WRITE flags.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // The callback was called ONCE, with both READ and WRITE flags.
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_WRITE);
    assert_true(data.emask & EVIO_READ);
    assert_int_equal(loop->fds.ptr[fd].gen, 2);

    // Clean up the pipe from the write in step 2 and reset state.
    char buf[1];
    read(fd, buf, sizeof(buf));
    data.called = 0;
    data.emask = 0;

    // gen++.
    evio_poll_change(loop, &io, fd, EVIO_READ);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process the change.
    assert_int_equal(data.called, 0); // No event yet.
    assert_int_equal(loop->fds.ptr[fd].gen, 3);

    assert_int_equal(write(fds[1], "y", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);

    // Now the callback for the fresh READ event is called, and only for READ.
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_READ);
    assert_false(data.emask & EVIO_WRITE);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_stale_event_no_watchers_invalidate)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;

    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT);

    // Trigger event.
    assert_int_equal(write(fds[1], "x", 1), 1);

    evio_poll_stop(loop, &io);

    // Run the loop. It will process the DEL change.
    // It will also get the stale event from epoll_wait.
    // The handler will call evio_invalidate_fd, which returns 0
    // because there are no more watchers.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callback was called.
    assert_int_equal(data.called, 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_wait_ebadf_abort)
{
    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    // Close the epoll fd to force an error in epoll_pwait
    int fd_to_close = loop->fd;
    loop->fd = -1; // EBADF, avoid double-close.
    close(fd_to_close);

    if (setjmp(jmp) == 0) {
        evio_run(loop, EVIO_RUN_ONCE);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(abort_ctx.called, 1);
    evio_test_abort_ctx_end(&abort_ctx);
    evio_loop_free(loop);
}

TEST(test_evio_poll_stop_shared_fd)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io1;
    evio_poll_init(&io1, generic_cb, fds[0], EVIO_READ);
    io1.data = &data;

    evio_poll io2;
    evio_poll_init(&io2, generic_cb, fds[0], EVIO_READ);
    io2.data = &data;

    evio_poll_start(loop, &io1);
    evio_poll_start(loop, &io2);
    assert_int_equal(evio_refcount(loop), 2);

    // Stop one of them. evio_invalidate_fd returns > 0 because p2 is still active.
    // This triggers a new change notification.
    evio_poll_stop(loop, &io1);
    assert_int_equal(evio_refcount(loop), 1);
    assert_int_equal(loop->fdchanges.count, 1);

    evio_run(loop, EVIO_RUN_NOWAIT);

    // p2 stays active.
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);

    evio_poll_stop(loop, &io2);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_error_and_stop)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process add

    // Queue an error for the fd
    evio_queue_fd_error(loop, fds[0]);
    assert_int_equal(loop->fderrors.count, 1);

    // Stop watcher with pending error.
    evio_poll_stop(loop, &io);
    assert_int_equal(evio_refcount(loop), 0);

    // Process fderrors list; no watchers => flush.
    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(loop->fderrors.count, 0);
    assert_int_equal(data.called, 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_resizing_and_invalidation)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_poll io[5];
    int fds[5][2];

    for (int i = 0; i < 3; ++i) {
        fds[i][0] = fds[i][1] = -1;
        assert_int_equal(pipe(fds[i]), 0);
        evio_poll_init(&io[i], generic_cb, fds[i][0], EVIO_READ);
        io[i].data = &data;
        evio_poll_start(loop, &io[i]);
    }
    assert_int_equal(loop->refcount, 3);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process changes

    // Middle removal.
    evio_poll_stop(loop, &io[1]);
    assert_int_equal(loop->refcount, 2);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process changes

    // Write to the remaining active pipes, ensure they work.
    data.called = 0;
    assert_int_equal(write(fds[0][1], "x", 1), 1);
    assert_int_equal(write(fds[2][1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 2);

    // Close a stopped watcher fd, then stop another watcher (invalidate path).
    close(fds[1][0]);
    close(fds[1][1]);
    evio_poll_stop(loop, &io[0]);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(loop->refcount, 1);

    // Cleanup
    evio_poll_stop(loop, &io[2]);
    close(fds[0][0]);
    close(fds[0][1]);
    close(fds[2][0]);
    close(fds[2][1]);

    evio_loop_free(loop);
}

TEST(test_evio_poll_invalid_fd_asserts)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);
    int valid_fd = fds[0];

    evio_poll io;
    expect_assert_failure(evio_poll_init(&io, dummy_cb, -1, EVIO_READ));

    evio_init(&io.base, dummy_cb);
    io.fd = -1; // Corrupt fd before start
    io.emask = EVIO_READ;
    expect_assert_failure(evio_poll_start(loop, &io));

    evio_poll_init(&io, dummy_cb, valid_fd, EVIO_READ);
    evio_poll_start(loop, &io);
    assert_true(io.active);

    int original_fd = io.fd;
    io.fd = -1;
    expect_assert_failure(evio_poll_change(loop, &io, -1, EVIO_READ));
    io.fd = loop->fds.count;
    expect_assert_failure(evio_poll_change(loop, &io, loop->fds.count, EVIO_READ));
    io.fd = original_fd; // Restore

    io.fd = -1; // Corrupt fd
    expect_assert_failure(evio_poll_stop(loop, &io));
    io.fd = original_fd; // Restore

    io.fd = loop->fds.count;
    expect_assert_failure(evio_poll_stop(loop, &io));
    io.fd = original_fd; // Restore

    // The watcher is still active because previous calls aborted. Stop it cleanly.
    evio_poll_stop(loop, &io);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_stale_event_gen_mismatch)
{
    generic_cb_data data1 = { 0 };
    generic_cb_data data2 = { 0 };

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    int fd = fds[0];

    evio_poll io1;
    // Use two different callbacks to distinguish the watchers
    evio_poll_init(&io1, generic_cb, fd, EVIO_READ);
    io1.data = &data1;

    evio_poll io2;
    evio_poll_init(&io2, generic_cb, fd, EVIO_WRITE);
    io2.data = &data2;

    evio_poll_start(loop, &io1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(loop->fds.ptr[fd].gen, 1);

    // Trigger a READ event. Kernel now has a pending READ event for gen=1.
    assert_int_equal(write(fds[1], "x", 1), 1);

    // This forces the event mask to change, which will increment the generation.
    evio_poll_stop(loop, &io1);
    evio_poll_start(loop, &io2);

    // Run the loop once.
    // - evio_poll_update will see the mask changed from READ to WRITE,
    //   call epoll_ctl(MOD), and increment gen to 2.
    // - evio_poll_wait will receive both the stale READ event (for gen=1) and
    //   the fresh WRITE event (for gen=2).
    // - discard READ event, process WRITE.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // Stale watcher callback does not fire.
    assert_int_equal(data1.called, 0);

    // The callback for the new watcher (io2) SHOULD have been called.
    assert_int_equal(data2.called, 1);

    // Cleanup
    char buf[1];
    read(fd, buf, sizeof(buf)); // consume the data

    evio_poll_stop(loop, &io2);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_event_with_pending_change)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;

    // Start watcher and process initial change manually
    evio_poll_start(loop, &io);
    evio_poll_update(loop);
    assert_int_equal(loop->fdchanges.count, 0);

    // Trigger an event and queue a change for the same fd
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_poll_change(loop, &io, fds[0], EVIO_READ | EVIO_WRITE);
    assert_true(loop->fdchanges.count > 0);

    // Call poll_wait manually. It will receive the event, but since a change is pending
    // drop the event.
    evio_poll_wait(loop, 0);

    // No event was queued.
    evio_invoke_pending(loop);
    assert_int_equal(data.called, 0);

    // Now, process the pending change manually.
    evio_poll_update(loop);
    assert_int_equal(loop->fdchanges.count, 0);

    // The socket is still readable. Call poll_wait again.
    // This time, no change is pending, so the event is queued.
    evio_poll_wait(loop, 0);
    evio_invoke_pending(loop);
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_READ);
    assert_true(data.emask & EVIO_WRITE);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

static void error_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    if (emask & EVIO_ERROR) {
        size_t *counter = base->data;
        ++(*counter);
    }
}

TEST(test_evio_poll_update_ebadf)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, error_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    size_t counter = 0;
    io.data = &counter;

    // Close the fd before the change is processed by evio_poll_update
    close(fds[0]);

    // This run will call evio_poll_update, which will try epoll_ctl(ADD) on
    // the closed fd. This fails with EBADF, which is the default case in the
    // switch, which then calls evio_queue_fd_errors.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // evio_queue_fd_errors stops the watcher and queues an error event.
    assert_int_equal(counter, 1);
    assert_false(io.active);

    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_error_cb_no_error)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    // Use error_cb for a normal poll watcher
    evio_poll_init(&io, error_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    size_t counter = 0;
    io.data = &counter;

    // Trigger a normal read event
    assert_int_equal(write(fds[1], "x", 1), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);

    // The callback is called, but the if (emask & EVIO_ERROR) is false.
    // So counter is 0.
    assert_int_equal(counter, 0);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_spurious_event)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    // Run once to add the fd to epoll with EVIO_READ
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

TEST(test_evio_poll_spurious_event_del)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    int fd = fds[0];

    evio_poll io;
    evio_poll_init(&io, generic_cb, fd, EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);

    // Register with epoll
    evio_run(loop, EVIO_RUN_NOWAIT);

    // Manually desync epoll state to trigger spurious event logic
    struct epoll_event ev = { .events = EPOLLIN | EPOLLOUT };
    ev.data.u64 = ((uint64_t)fd) | ((uint64_t)loop->fds.ptr[fd].gen << 32);
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_MOD, fd, &ev), 0);

    // Manually set the library's internal mask for this fd to 0. This is a
    // white-box test to simulate a state where all watchers are gone but the
    // kernel registration is stale.
    loop->fds.ptr[fd].emask = 0;

    // Run the loop. It will receive the spurious EPOLLOUT event.
    // The handler will see fds->emask is 0 and choose op=EPOLL_CTL_DEL,
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callback is called because the watcher was conceptually stopped.
    assert_int_equal(data.called, 0);

    // The watcher is still technically active in the fds->list, so stop it
    // to clean up correctly.
    evio_poll_stop(loop, &io);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_eexist_mod_fail)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    // Start and run a watcher to set the initial fds state (emask=READ, cache=READ).
    evio_poll io1;
    evio_poll_init(&io1, generic_cb, fds[0], EVIO_READ);
    io1.data = &data;
    evio_poll_start(loop, &io1);
    evio_run(loop, EVIO_RUN_NOWAIT);

    // Stop and run to clear the epoll registration from evio's perspective.
    evio_poll_stop(loop, &io1);
    evio_run(loop, EVIO_RUN_NOWAIT);

    // Manually add the FD back to epoll to create a desync.
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_ADD, fds[0], &ev), 0);

    // Start a new watcher with a different mask (WRITE). This queues a change.
    evio_poll io2;
    evio_poll_init(&io2, error_cb, fds[0], EVIO_WRITE);
    evio_poll_start(loop, &io2);

    size_t counter = 0;
    io2.data = &counter;

    // Close the fd before running the loop.
    close(fds[0]);

    // Run. poll_update will:
    // - op=ADD (old emask was 0), epoll_ctl(ADD) fails with EEXIST.
    // - case EEXIST: cache(READ) != new_cache(WRITE).
    // - epoll_ctl(MOD) on closed fd fails with EBADF.
    // - break is hit -> evio_queue_fd_errors is called.
    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_invoke_pending(loop);

    assert_int_equal(counter, 1);
    assert_false(io2.active);

    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_enoent_add_fail)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, error_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    size_t counter = 0;
    io.data = &counter;

    evio_run(loop, EVIO_RUN_NOWAIT);

    // Manually remove the FD from epoll to create a desync.
    struct epoll_event ev = { 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_DEL, fds[0], &ev), 0);

    // Close the fd and then queue a change.
    close(fds[0]);
    evio_poll_change(loop, &io, fds[0], EVIO_WRITE);

    // Run. poll_update will:
    // - op=MOD, epoll_ctl(MOD) fails with ENOENT.
    // - case ENOENT:
    // - epoll_ctl(ADD) on closed fd fails with EBADF.
    // - break is hit -> evio_queue_fd_errors is called.
    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_invoke_pending(loop);

    assert_int_equal(counter, 1);
    assert_false(io.active);

    close(fds[1]);
    evio_loop_free(loop);
}

// Stale event after stop: invalidate_fd hits ENOENT.
TEST(test_evio_poll_stale_event_after_stop)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Add to epoll

    // Trigger an event so it's in the kernel's ready list.
    assert_int_equal(write(fds[1], "x", 1), 1);

    // Stop the watcher.
    // This calls evio_invalidate_fd, which removes the fd from epoll.
    evio_poll_stop(loop, &io);

    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

// Stale event after last watcher removed: invalidate_fd succeeds.
TEST(test_evio_poll_stale_event_clean_del)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Add to epoll

    // Trigger an event so it's in the kernel's ready list.
    assert_int_equal(write(fds[1], "x", 1), 1);

    // Manually remove the watcher from the loop's internal lists,
    // but leave the fd registered in epoll. This creates the state
    // where a stale event can be processed for an fd with no watchers.
    loop->fds.ptr[fds[0]].list.count = 0;
    evio_unref(loop);
    io.active = 0;

    // Run the loop. epoll_wait will return the stale event.
    // evio_poll_wait will call evio_invalidate_fd.
    // Inside evio_invalidate_fd, list.count is 0, and epoll_ctl(DEL) will
    // succeed, causing it to return 0.
    // The check `if (evio_invalidate_fd(...) <= 0)` will be true,
    // and the event will be correctly ignored.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callback was called.
    assert_int_equal(data.called, 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_stop_on_closed_fd_asserts)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, dummy_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Run once to get the fd registered with epoll.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // Close the fd before stopping the watcher.
    close(fds[0]);

    // DEL on closed fd => EBADF, evio_invalidate_fd returns -1.
    expect_assert_failure(evio_poll_stop(loop, &io));

    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_stop_middle_updates_active)
{
    generic_cb_data data[3] = { {0}, {0}, {0} };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io[3];
    for (int i = 0; i < 3; i++) {
        evio_poll_init(&io[i], generic_cb, fds[0], EVIO_READ);
        io[i].data = &data[i];
        evio_poll_start(loop, &io[i]);
    }

    assert_int_equal(io[0].base.active, 1);
    assert_int_equal(io[1].base.active, 2);
    assert_int_equal(io[2].base.active, 3);

    assert_int_equal(loop->fds.ptr[fds[0]].list.count, 3);

    evio_poll_stop(loop, &io[1]);
    assert_false(io[1].base.active);

    assert_int_equal(io[2].base.active, 2);

    assert_int_equal(loop->fds.ptr[fds[0]].list.count, 2);

    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data[0].called, 1);
    assert_int_equal(data[1].called, 0);  // stopped
    assert_int_equal(data[2].called, 1);

    evio_poll_stop(loop, &io[0]);
    evio_poll_stop(loop, &io[2]);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_get_fd)
{
    evio_poll io;
    evio_poll_init(&io, dummy_cb, 42, EVIO_READ);

    assert_int_equal(evio_poll_get_fd(&io), 42);

    evio_poll_set(&io, 123, EVIO_WRITE);
    assert_int_equal(evio_poll_get_fd(&io), 123);
}

TEST(test_evio_poll_get_events)
{
    evio_poll io;
    evio_poll_init(&io, dummy_cb, 0, EVIO_READ);
    assert_int_equal(evio_poll_get_events(&io), EVIO_READ);

    evio_poll_modify(&io, EVIO_WRITE);
    assert_int_equal(evio_poll_get_events(&io), EVIO_WRITE);

    evio_poll_modify(&io, EVIO_READ | EVIO_WRITE);
    assert_int_equal(evio_poll_get_events(&io), EVIO_READ | EVIO_WRITE);

    evio_poll_modify(&io, 0);
    assert_int_equal(evio_poll_get_events(&io), 0);
}
