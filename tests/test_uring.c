#include "test.h"

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
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

#ifdef EVIO_IO_URING
    // GCOVR_EXCL_START
    if (!loop->iou) {
        print_message("      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
    // GCOVR_EXCL_STOP
#else
    print_message("      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    int fds1[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1), 0);

    int fds2[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds1[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Trigger on first pipe
    assert_int_equal(write(fds1[1], "a", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
    reset_cb_state();

    // Change to watch for write on the same fd
    char buf[1];
    assert_int_equal(read(fds1[0], buf, 1), 1); // clear pipe
    evio_poll_change(loop, &io, fds1[0], EVIO_WRITE);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
    reset_cb_state();

    // Change to watch for read on the second pipe
    evio_poll_change(loop, &io, fds2[0], EVIO_READ);
    assert_int_equal(write(fds2[1], "c", 1), 1); // write to new pipe
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
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
    assert_non_null(loop);

#ifdef EVIO_IO_URING
    // GCOVR_EXCL_START
    if (!loop->iou) {
        print_message("      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
    // GCOVR_EXCL_STOP
#else
    print_message("      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    int fd = open("/dev/null", O_RDWR);
    assert_true(fd >= 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fd, EVIO_READ | EVIO_WRITE);
    evio_poll_start(loop, &io);

    // The first run will submit an io_uring epoll_ctl, which will fail with EPERM.
    // The library should queue an fd error.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // The callback should have been invoked with a poll event because of the error.
    assert_int_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_POLL);
    assert_true(generic_cb_emask & EVIO_READ);
    assert_true(generic_cb_emask & EVIO_WRITE);
    assert_false(generic_cb_emask & EVIO_ERROR);

    assert_true(io.active);
    assert_int_equal(evio_refcount(loop), 1);

    evio_poll_stop(loop, &io);
    close(fd);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_eexist_error)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

#ifdef EVIO_IO_URING
    // GCOVR_EXCL_START
    if (!loop->iou) {
        print_message("      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
    // GCOVR_EXCL_STOP
#else
    print_message("      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    // Manually add the fd to the epoll set.
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_ADD, fds[0], &ev), 0);

    // Now, have evio start a watcher. It will try to ADD via uring, fail with EEXIST,
    // and should recover by retrying with MOD via uring.
    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process the change

    // The watcher should be active and working.
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_spurious_event)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

#ifdef EVIO_IO_URING
    // GCOVR_EXCL_START
    if (!loop->iou) {
        print_message("      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
    // GCOVR_EXCL_STOP
#else
    print_message("      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Run once to add the fd to epoll with EVIO_READ via uring
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0);

    // Manually change the epoll registration to include EPOLLOUT.
    // This creates a mismatch between what evio thinks it's watching
    // and what the kernel is actually watching.
    struct epoll_event ev = { .events = EPOLLIN | EPOLLOUT };
    ev.data.u64 = ((uint64_t)fds[0]) | ((uint64_t)loop->fds.ptr[fds[0]].gen << 32);
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_MOD, fds[0], &ev), 0);

    // Run the loop. The socket is writeable, so epoll_pwait will return an
    // EPOLLOUT event. evio_poll_wait will see this event (EVIO_WRITE) which
    // is not in its own mask (~fds->emask). This triggers the target code path.
    // op will be MOD, and the `else` branch will call evio_uring_ctl.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callback should be called, because the watcher is only for EVIO_READ
    // and the spurious event was EVIO_WRITE.
    assert_int_equal(generic_cb_called, 0);

    // Verify the internal state was corrected.
    assert_int_equal(loop->fds.ptr[fds[0]].emask, EVIO_READ);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_enoent_error)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

#ifdef EVIO_IO_URING
    // GCOVR_EXCL_START
    if (!loop->iou) {
        print_message("      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
    // GCOVR_EXCL_STOP
#else
    print_message("      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Add fd via uring

    // Manually remove the fd from epoll to create an inconsistent state.
    struct epoll_event ev = { 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_DEL, fds[0], &ev), 0);

    // Now, change the watcher. It will try to MOD, fail with ENOENT,
    // and should recover by using ADD.
    evio_poll_change(loop, &io, fds[0], EVIO_READ | EVIO_WRITE);
    // This run should process the change, recover from the ENOENT error by
    // re-adding the fd, and then receive the write-ready event.
    evio_run(loop, EVIO_RUN_ONCE);
    assert_int_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_WRITE);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

#define MANY_URING_EVENTS 1024
#define EVIO_URING_EVENTS 256

TEST(test_evio_poll_uring_autoflush)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

#ifdef EVIO_IO_URING
    // GCOVR_EXCL_START
    if (!loop->iou) {
        print_message("      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
    // GCOVR_EXCL_STOP
#else
    print_message("      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    evio_poll io[EVIO_URING_EVENTS];
    int fds[EVIO_URING_EVENTS][2];

    // This loop should trigger the auto-flush logic in evio_uring_ctl
    // when the submission queue becomes full.
    for (size_t i = 0; i < EVIO_URING_EVENTS; ++i) {
        fds[i][0] = fds[i][1] = -1;
        assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]), 0);
        evio_poll_init(&io[i], generic_cb, fds[i][0], EVIO_READ);
        evio_poll_start(loop, &io[i]);
    }

    // After this, all watchers should be active.
    assert_int_equal(evio_refcount(loop), EVIO_URING_EVENTS);

    // Trigger one event to check that things are working.
    assert_int_equal(write(fds[0][1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);

    // Cleanup
    for (size_t i = 0; i < EVIO_URING_EVENTS; ++i) {
        evio_poll_stop(loop, &io[i]);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    evio_loop_free(loop);
}

static size_t drain_uring_events_loop(evio_loop *loop, size_t expected_calls, size_t loop_limit)
{
    size_t loops = 0;
    while (generic_cb_called < expected_calls && loops < loop_limit) {
        evio_run(loop, EVIO_RUN_NOWAIT);
        loops++;
    }
    return loops;
}

TEST(test_evio_poll_uring_many_events)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

#ifdef EVIO_IO_URING
    // GCOVR_EXCL_START
    if (!loop->iou) {
        print_message("      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
    // GCOVR_EXCL_STOP
#else
    print_message("      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    evio_poll io[MANY_URING_EVENTS];
    int fds[MANY_URING_EVENTS][2];

    // 1. Setup phase: initialize and start all watchers.
    // This will repeatedly fill and flush the io_uring submission queue.
    for (size_t i = 0; i < MANY_URING_EVENTS; ++i) {
        fds[i][0] = fds[i][1] = -1;
        assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]), 0);
        evio_poll_init(&io[i], read_and_count_cb, fds[i][0], EVIO_READ);
        evio_poll_start(loop, &io[i]);
    }

    // 2. Flush phase: ensure all initial ADD operations are processed.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0);

    // 3. Event generation phase: write to all sockets to make them readable.
    for (size_t i = 0; i < MANY_URING_EVENTS; ++i) {
        assert_int_equal(write(fds[i][1], "x", 1), 1);
    }

    // 4. Processing phase: drain all pending events from the loop.
    drain_uring_events_loop(loop, MANY_URING_EVENTS, MANY_URING_EVENTS * 2);
    assert_int_equal(generic_cb_called, MANY_URING_EVENTS);

    // 5. Cleanup phase.
    for (size_t i = 0; i < MANY_URING_EVENTS; ++i) {
        evio_poll_stop(loop, &io[i]);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_many_events_loop_break)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

#ifdef EVIO_IO_URING
    // GCOVR_EXCL_START
    if (!loop->iou) {
        print_message("      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
    // GCOVR_EXCL_STOP
#else
    print_message("      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);
    evio_poll io;

    evio_poll_init(&io, read_and_count_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0);

    // Don't trigger any events. The loop should time out.
    size_t loops = drain_uring_events_loop(loop, 1, 5);
    assert_int_equal(loops, 5);
    assert_int_equal(generic_cb_called, 0);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_uring_ebadf_error)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
    assert_non_null(loop);

#ifdef EVIO_IO_URING
    // GCOVR_EXCL_START
    if (!loop->iou) {
        print_message("      -> Skipping test, io_uring not supported by kernel\n");
        evio_loop_free(loop);
        return;
    }
    // GCOVR_EXCL_STOP
#else
    print_message("      -> Skipping test, io_uring not compiled\n");
    evio_loop_free(loop);
    return;
#endif

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process ADD via uring

    // Close the fd, then queue a change.
    close(fds[0]);
    evio_poll_change(loop, &io, fds[0], EVIO_READ | EVIO_WRITE);

    // Run the loop. This will submit the MOD via uring. The kernel will try
    // epoll_ctl, which fails with EBADF. The cqe->res will be -EBADF, hitting
    // the default case in evio_uring_flush, which calls evio_queue_fd_errors.
    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_invoke_pending(loop);

    // evio_queue_fd_errors stops the watcher and queues an error event.
    assert_int_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_ERROR);
    assert_false(io.active);

    close(fds[1]);
    evio_loop_free(loop);
}
