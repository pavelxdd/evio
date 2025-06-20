#include "test.h"

TEST(test_evio_poll)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Double start should be a no-op
    evio_poll_start(loop, &io);
    assert_int_equal(evio_refcount(loop), 1);

    // Write to the pipe to trigger a read event
    assert_int_equal(write(fds[1], "x", 1), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_READ);

    evio_poll_stop(loop, &io);
    assert_int_equal(evio_refcount(loop), 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_change)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds1[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1), 0);

    int fds2[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2), 0);

    evio_poll io;
    // Use a callback that consumes the read event to avoid level-triggering issues.
    evio_poll_init(&io, read_and_count_cb, fds1[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Trigger on first pipe
    assert_int_equal(write(fds1[1], "a", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
    reset_cb_state();

    // Change to watch for write on the same fd.
    // Switch to a non-reading callback for this part.
    evio_poll_change(loop, &io, fds1[0], EVIO_WRITE);
    io.base.cb = generic_cb;
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_WRITE);
    reset_cb_state();

    // Change to watch for read on the second pipe.
    // Switch back to the reading callback.
    evio_poll_change(loop, &io, fds2[0], EVIO_READ);
    io.base.cb = read_and_count_cb;

    assert_int_equal(write(fds1[1], "b", 1), 1); // write to old pipe
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0); // should not trigger

    assert_int_equal(write(fds2[1], "c", 1), 1); // write to new pipe
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
    reset_cb_state();

    // Change with same mask should be a no-op. Since the data was read,
    // the fd is no longer readable, so no event should fire.
    evio_poll_change(loop, &io, fds2[0], EVIO_READ);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0);

    // Change to a new fd with emask=0. This should stop the watcher.
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
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);
    assert_int_equal(evio_refcount(loop), 1);

    // Change with emask=0 should stop the watcher.
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
    evio_poll_init(&w, generic_cb, 0, 0);
    w.emask = EVIO_READ | EVIO_POLL;

    // Modify to WRITE, should preserve POLL
    evio_poll_modify(&w, EVIO_WRITE);
    assert_int_equal(w.emask, EVIO_WRITE | EVIO_POLL);

    // Modify with extra flags, should only take READ/WRITE
    evio_poll_modify(&w, EVIO_READ | EVIO_TIMER);
    assert_int_equal(w.emask, EVIO_READ | EVIO_POLL);

    // Modify to 0, should preserve POLL
    evio_poll_modify(&w, 0);
    assert_int_equal(w.emask, EVIO_POLL);
}

static void *eintr_thread_func(void *arg)
{
    // Let main thread enter epoll_pwait
    usleep(50000); // 50ms
    pthread_kill(*(pthread_t *)arg, SIGUSR1);
    return NULL;
}

// Do nothing, just to interrupt syscall
static void sigusr1_handler(int signum) {}

TEST(test_evio_poll_wait_eintr)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Setup a dummy signal handler for SIGUSR1
    struct sigaction sa_new = { .sa_handler = sigusr1_handler };
    struct sigaction sa_old;
    sigaction(SIGUSR1, &sa_new, &sa_old);

    pthread_t main_thread_id = pthread_self();
    pthread_t killer_thread;
    pthread_create(&killer_thread, NULL, eintr_thread_func, &main_thread_id);

    // This will call epoll_pwait with a timeout. The other thread will
    // interrupt it with a signal. The loop in evio_poll_wait should
    // handle EINTR and continue. The loop has no active watchers with refcount,
    // so it will run once and exit.
    evio_run(loop, EVIO_RUN_ONCE);

    pthread_join(killer_thread, NULL);

    // Restore default handler
    sigaction(SIGUSR1, &sa_old, NULL);

    evio_loop_free(loop);
}

TEST(test_evio_feed_fd)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Feed an event manually for the fd.
    evio_feed_fd_event(loop, fds[0], EVIO_READ);

    // Run and check that the callback was called.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_READ);

    reset_cb_state();

    // Feed an error manually for the fd.
    evio_feed_fd_error(loop, fds[0]);

    // This should stop the watcher and call the callback with an error.
    evio_invoke_pending(loop);
    assert_int_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_ERROR);
    assert_false(io.active);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_update_empty_emask)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    // Initialize with no events. This will set EVIO_POLL flag internally.
    evio_poll_init(&io, generic_cb, fds[0], 0);

    // Start the watcher. This queues a change, and then clears the EVIO_POLL
    // flag from the watcher's emask, leaving it as 0.
    evio_poll_start(loop, &io);
    assert_int_equal(io.emask, 0);
    assert_int_equal(loop->fdchanges.count, 1);

    // Run the loop. evio_poll_update will be called.
    // It will find the change, iterate watchers for the fd.
    // The only watcher has emask 0, so the calculated fds->emask will be 0.
    // This will hit the `if (!fds->emask)` branch and `continue`, covering the line.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callback should have been called because nothing was watched.
    assert_int_equal(generic_cb_called, 0);
    // The change should have been processed.
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
    assert_int_equal(generic_cb_called, 1);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_change_inactive)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    // Watcher is initialized but not started, so it's inactive.

    // Calling change on an inactive watcher should start it.
    evio_poll_change(loop, &io, fds[0], EVIO_READ);

    // The watcher should now be active.
    assert_true(io.active);
    assert_int_equal(evio_refcount(loop), 1);

    // Trigger an event to make sure it's working.
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

static size_t drain_events_loop(evio_loop *loop, size_t expected_calls, size_t loop_limit)
{
    size_t loops = 0;
    while (generic_cb_called < expected_calls && loops < loop_limit) {
        evio_run(loop, EVIO_RUN_NOWAIT);
        loops++;
    }
    return loops;
}

#define MANY_FDS 100
TEST(test_evio_poll_many_events)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_poll io[MANY_FDS];
    int fds[MANY_FDS][2];

    // 1. Setup phase: initialize and start all watchers.
    for (int i = 0; i < MANY_FDS; ++i) {
        fds[i][0] = fds[i][1] = -1;
        assert_int_equal(pipe(fds[i]), 0);
        // Use the read_and_count_cb to consume the event
        evio_poll_init(&io[i], read_and_count_cb, fds[i][0], EVIO_READ);
        // Store the read-end of the pipe in the watcher's data field
        evio_poll_start(loop, &io[i]);
    }

    // 2. Flush phase: run the loop once to ensure evio_poll_update() runs
    // and all fds are added to epoll. We don't expect events yet.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0);

    // 3. Event generation phase: write to all pipes to make them readable.
    for (int i = 0; i < MANY_FDS; ++i) {
        assert_int_equal(write(fds[i][1], "x", 1), 1);
    }

    // 4. Processing phase: drain all pending events from the loop.
    drain_events_loop(loop, MANY_FDS, MANY_FDS * 2);
    assert_int_equal(generic_cb_called, MANY_FDS);

    // 5. Cleanup phase.
    for (int i = 0; i < MANY_FDS; ++i) {
        evio_poll_stop(loop, &io[i]);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    evio_loop_free(loop);
}

TEST(test_evio_poll_many_events_loop_break)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, read_and_count_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0);

    // Don't trigger any events. The loop should time out.
    size_t loops = drain_events_loop(loop, 1, 5);
    assert_int_equal(loops, 5);
    assert_int_equal(generic_cb_called, 0);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_enoent_error)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Add fd to epoll

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

// This test covers the EPERM error path in evio_poll_update, which happens
// when trying to poll a file descriptor that doesn't support polling (e.g., a regular file).
TEST(test_evio_poll_eperm_error)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fd = open("/dev/null", O_RDWR);
    assert_true(fd >= 0);

    evio_poll io;
    // Watch for both read and write, as /dev/null is always ready for both.
    evio_poll_init(&io, generic_cb, fd, EVIO_READ | EVIO_WRITE);
    evio_poll_start(loop, &io); // This will queue a change.

    // The first run will call epoll_ctl, which will fail with EPERM.
    // The library should treat this as the fd being permanently ready.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // The callback should have been invoked with a normal poll event.
    assert_int_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_POLL);
    assert_true(generic_cb_emask & EVIO_READ);
    assert_true(generic_cb_emask & EVIO_WRITE);
    assert_false(generic_cb_emask & EVIO_ERROR);

    // The watcher should still be active.
    assert_true(io.active);
    assert_int_equal(evio_refcount(loop), 1);

    evio_poll_stop(loop, &io);
    close(fd);
    evio_loop_free(loop);
}

TEST(test_evio_poll_eexist_error)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    // Manually add the fd to the epoll set to create an inconsistent state.
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_ADD, fds[0], &ev), 0);

    // Now, have evio start a watcher. It will try to ADD, fail with EEXIST,
    // and should recover by using MOD.
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

TEST(test_evio_poll_update_no_change)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io1, io2;
    evio_poll_init(&io1, generic_cb, fds[0], EVIO_READ);
    evio_poll_init(&io2, generic_cb2, fds[0], EVIO_READ);

    // Start two watchers on the same fd.
    evio_poll_start(loop, &io1);
    evio_poll_start(loop, &io2);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process ADD.

    // Stop one watcher. This queues a change, but the resulting event mask
    // for the fd is the same (still EVIO_READ).
    evio_poll_stop(loop, &io1);

    // Run the loop. This will execute poll_update, which should hit the
    // optimization branch and do nothing.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // The remaining watcher should still work.
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 0);  // io1's cb
    assert_int_equal(generic_cb2_called, 1); // io2's cb

    evio_poll_stop(loop, &io2);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_eexist_same_mask)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);

    // 1. Start, run to add to epoll. emask is now READ.
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(loop->fds.ptr[fds[0]].emask, EVIO_READ);

    // 2. Stop, run to remove from epoll. emask is now 0.
    evio_poll_stop(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(loop->fds.ptr[fds[0]].emask, 0);

    // 3. Manually add fd back to epoll to create desync.
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_ADD, fds[0], &ev), 0);

    // 4. Start watcher again. This queues a change.
    evio_poll_start(loop, &io);

    // 5. Run. poll_update will:
    //    - See old emask was 0, so op=ADD.
    //    - epoll_ctl(ADD) fails with EEXIST.
    //    - The recovery logic will then call epoll_ctl(MOD), which should succeed.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // Watcher should be working.
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_gen_counter_robustness)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    int fd = fds[0];

    evio_poll io;
    evio_poll_init(&io, generic_cb, fd, EVIO_READ);

    // 1. Start and run. gen becomes 1.
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(loop->fds.ptr[fd].gen, 1);

    // 2. Trigger a READ event by writing to the pipe.
    assert_int_equal(write(fds[1], "x", 1), 1);

    // 3. Force a change by modifying the event mask. This queues a change.
    evio_poll_change(loop, &io, fd, EVIO_READ | EVIO_WRITE);

    // 4. Run the loop.
    // - poll_update will call epoll_ctl(MOD), incrementing gen to 2.
    // - The fd is still readable from step 2, and is also writeable.
    // - epoll_wait returns a single event for gen=2 with events EPOLLIN | EPOLLOUT.
    // - The gen check passes, and the callback is invoked with both READ and WRITE flags.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // The callback should have been called ONCE, with both READ and WRITE flags.
    assert_int_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_WRITE);
    assert_true(generic_cb_emask & EVIO_READ);
    assert_int_equal(loop->fds.ptr[fd].gen, 2);

    // 5. Clean up the pipe from the write in step 2 and reset state.
    char buf[1];
    read(fd, buf, sizeof(buf));
    reset_cb_state();

    // 6. Change watcher back to only watch for READ. This will increment gen to 3.
    evio_poll_change(loop, &io, fd, EVIO_READ);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process the change.
    assert_int_equal(generic_cb_called, 0); // No event should fire yet.
    assert_int_equal(loop->fds.ptr[fd].gen, 3);

    // 7. Trigger a new READ event. This will be for the current gen=3.
    assert_int_equal(write(fds[1], "y", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);

    // 8. Now the callback for the fresh READ event should be called, and only for READ.
    assert_int_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_READ);
    assert_false(generic_cb_emask & EVIO_WRITE);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_stale_event_no_watchers_invalidate)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);

    // 1. Start watcher and run to add to epoll.
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT);

    // 2. Trigger event.
    assert_int_equal(write(fds[1], "x", 1), 1);

    // 3. Stop the watcher.
    evio_poll_stop(loop, &io);

    // 4. Run the loop. It will process the DEL change.
    // It will also get the stale event from epoll_wait.
    // The handler will call evio_invalidate_fd, which should return 0
    // because there are no more watchers. This hits the target branch.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callback should have been called.
    assert_int_equal(generic_cb_called, 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_stop_shared_fd)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io1, io2;
    evio_poll_init(&io1, generic_cb, fds[0], EVIO_READ);
    evio_poll_init(&io2, generic_cb, fds[0], EVIO_READ);

    evio_poll_start(loop, &io1);
    evio_poll_start(loop, &io2);
    assert_int_equal(evio_refcount(loop), 2);

    // Stop one of them. evio_invalidate_fd should return > 0 because p2 is still active.
    // This should trigger a new change notification.
    evio_poll_stop(loop, &io1);
    assert_int_equal(evio_refcount(loop), 1);
    assert_int_equal(loop->fdchanges.count, 1);

    evio_run(loop, EVIO_RUN_NOWAIT);

    // p2 should still be active and working
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 1);

    evio_poll_stop(loop, &io2);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_error_and_stop)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process add

    // Queue an error for the fd
    evio_queue_fd_error(loop, fds[0]);
    assert_int_equal(loop->fderrors.count, 1);

    // Now stop the watcher. This will leave the error pending but no watcher on the fd.
    evio_poll_stop(loop, &io);
    assert_int_equal(evio_refcount(loop), 0);

    // Run the loop. evio_poll_wait should process the fderrors list.
    // Since there are no watchers for the fd (emask=0), it should flush the error.
    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(loop->fderrors.count, 0);
    assert_int_equal(generic_cb_called, 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_resizing_and_invalidation)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_poll io[5];
    int fds[5][2];

    // 1. Start 3 watchers
    for (int i = 0; i < 3; ++i) {
        fds[i][0] = fds[i][1] = -1;
        assert_int_equal(pipe(fds[i]), 0);
        evio_poll_init(&io[i], generic_cb, fds[i][0], EVIO_READ);
        evio_poll_start(loop, &io[i]);
    }
    assert_int_equal(loop->refcount, 3);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process changes

    // 2. Stop a watcher in the middle. This will trigger list-shrinking logic.
    evio_poll_stop(loop, &io[1]);
    assert_int_equal(loop->refcount, 2);
    evio_run(loop, EVIO_RUN_NOWAIT); // Process changes

    // 3. Write to the remaining active pipes, ensure they work.
    reset_cb_state();
    assert_int_equal(write(fds[0][1], "x", 1), 1);
    assert_int_equal(write(fds[2][1], "x", 1), 1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(generic_cb_called, 2);

    // 4. Invalidate the stopped watcher's fd by closing it, then stop another watcher.
    // This tests the evio_invalidate_fd path.
    close(fds[1][0]);
    close(fds[1][1]);
    evio_poll_stop(loop, &io[0]);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(loop->refcount, 1);

    // 5. Cleanup
    evio_poll_stop(loop, &io[2]);
    close(fds[0][0]);
    close(fds[0][1]);
    close(fds[2][0]);
    close(fds[2][1]);

    evio_loop_free(loop);
}

TEST(test_evio_poll_init_invalid_fd_assert)
{
    evio_poll io;
    expect_assert_failure(evio_poll_init(&io, generic_cb, -1, EVIO_READ));
}

TEST(test_evio_poll_change_invalid_fd_assert)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // This should trigger the assert in evio_poll_set
    expect_assert_failure(evio_poll_change(loop, &io, -1, EVIO_READ));

    // Cleanup
    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_change_active_invalid_fd_assert)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fd = open("/dev/null", O_RDONLY);
    assert_true(fd >= 0);

    evio_poll w;
    evio_poll_init(&w, generic_cb, fd, EVIO_READ);
    evio_poll_start(loop, &w);
    assert_true(w.active);

    int original_fd = w.fd;

    // Test with w->fd < 0
    w.fd = -1;
    expect_assert_failure(evio_poll_change(loop, &w, -1, EVIO_WRITE));
    w.fd = original_fd; // Restore

    // Test with w->fd >= loop->fds.count
    int invalid_fd = loop->fds.count;
    w.fd = invalid_fd;
    expect_assert_failure(evio_poll_change(loop, &w, invalid_fd, EVIO_WRITE));
    w.fd = original_fd; // Restore

    // The watcher is still active because the previous calls to evio_poll_change
    // aborted. We need to stop it cleanly.
    evio_poll_stop(loop, &w);

    close(fd);
    evio_loop_free(loop);
}

TEST(test_evio_poll_start_invalid_fd_assert)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_poll io;
    evio_init(&io.base, generic_cb);
    io.fd = -1;
    io.emask = EVIO_READ;
    expect_assert_failure(evio_poll_start(loop, &io));

    evio_loop_free(loop);
}

TEST(test_evio_poll_stop_invalid_fd_asserts)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fd = open("/dev/null", O_RDONLY);
    assert_true(fd >= 0);

    evio_poll w;
    evio_poll_init(&w, generic_cb, fd, EVIO_READ);
    evio_poll_start(loop, &w);
    assert_true(w.active);

    // Test w->fd < 0
    w.fd = -1;
    expect_assert_failure(evio_poll_stop(loop, &w));
    w.fd = fd; // Restore fd

    // Test w->fd >= loop->fds.count
    w.fd = loop->fds.count;
    expect_assert_failure(evio_poll_stop(loop, &w));
    w.fd = fd; // Restore fd

    // The watcher is still active because the previous calls to evio_poll_stop
    // aborted. We need to stop it cleanly.
    evio_poll_stop(loop, &w);

    close(fd);
    evio_loop_free(loop);
}

TEST(test_evio_poll_stale_event_gen_mismatch)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    int fd = fds[0];

    evio_poll io1, io2;
    // Use two different callbacks to distinguish the watchers
    evio_poll_init(&io1, generic_cb, fd, EVIO_READ);
    evio_poll_init(&io2, generic_cb2, fd, EVIO_WRITE);

    // 1. Start io1 (READ), run to add to epoll. gen becomes 1.
    evio_poll_start(loop, &io1);
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(loop->fds.ptr[fd].gen, 1);

    // 2. Trigger a READ event. Kernel now has a pending READ event for gen=1.
    assert_int_equal(write(fds[1], "x", 1), 1);

    // 3. Stop io1 and start io2 (WRITE) without running the loop.
    // This forces the event mask to change, which will increment the generation.
    evio_poll_stop(loop, &io1);
    evio_poll_start(loop, &io2);

    // 4. Run the loop once.
    // - evio_poll_update will see the mask changed from READ to WRITE,
    //   call epoll_ctl(MOD), and increment gen to 2.
    // - evio_poll_wait will receive both the stale READ event (for gen=1) and
    //   the fresh WRITE event (for gen=2).
    // - The handler should discard the READ event and process the WRITE event.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // The callback for the stale watcher (io1) should NOT have been called.
    assert_int_equal(generic_cb_called, 0);

    // The callback for the new watcher (io2) SHOULD have been called.
    assert_int_equal(generic_cb2_called, 1);

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
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);

    // 1. Start watcher and process initial change manually
    evio_poll_start(loop, &io);
    evio_poll_update(loop);
    assert_int_equal(loop->fdchanges.count, 0);

    // 2. Trigger an event and queue a change for the same fd
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_poll_change(loop, &io, fds[0], EVIO_READ | EVIO_WRITE);
    assert_true(loop->fdchanges.count > 0);

    // 3. Call poll_wait manually. It will receive the event, but since a change is pending
    //    for that fd, it should drop the event.
    evio_poll_wait(loop, 0);

    // 4. No event should have been queued.
    evio_invoke_pending(loop);
    assert_int_equal(generic_cb_called, 0);

    // 5. Now, process the pending change manually.
    evio_poll_update(loop);
    assert_int_equal(loop->fdchanges.count, 0);

    // 6. The socket is still readable. Call poll_wait again. This time, no change is pending,
    //    so the event should be queued.
    evio_poll_wait(loop, 0);
    evio_invoke_pending(loop);
    assert_int_equal(generic_cb_called, 1);
    assert_true(generic_cb_emask & EVIO_READ);
    assert_true(generic_cb_emask & EVIO_WRITE);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

static void error_cb(evio_loop *loop, evio_base *w, evio_mask emask)
{
    if (emask & EVIO_ERROR) {
        size_t *counter = w->data;
        ++(*counter);
    }
}

TEST(test_evio_poll_update_ebadf)
{
    reset_cb_state();
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
    // So counter should be 0.
    assert_int_equal(counter, 0);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_spurious_event)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    // Run once to add the fd to epoll with EVIO_READ
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
    // is not in its own mask (~fds->emask). This triggers the target code path,
    // which should silently correct the epoll registration.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callback should be called, because the watcher is only for EVIO_READ
    // and the spurious event was EVIO_WRITE.
    assert_int_equal(generic_cb_called, 0);

    // Verify the internal state was corrected. The emask should be unchanged
    // as the library corrects the kernel state to match its own.
    assert_int_equal(loop->fds.ptr[fds[0]].emask, EVIO_READ);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_eexist_mod_fail)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    // 1. Start and run a watcher to set the initial fds state (emask=READ, cache=READ).
    evio_poll io1;
    evio_poll_init(&io1, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io1);
    evio_run(loop, EVIO_RUN_NOWAIT);

    // 2. Stop and run to clear the epoll registration from evio's perspective.
    evio_poll_stop(loop, &io1);
    evio_run(loop, EVIO_RUN_NOWAIT);

    // 3. Manually add the FD back to epoll to create a desync.
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_ADD, fds[0], &ev), 0);

    // 4. Start a new watcher with a different mask (WRITE). This queues a change.
    evio_poll io2;
    evio_poll_init(&io2, error_cb, fds[0], EVIO_WRITE);
    evio_poll_start(loop, &io2);

    size_t counter = 0;
    io2.data = &counter;

    // 5. Close the fd before running the loop.
    close(fds[0]);

    // 6. Run. poll_update will:
    //    - op=ADD (old emask was 0), epoll_ctl(ADD) fails with EEXIST.
    //    - case EEXIST: cache(READ) != new_cache(WRITE).
    //    - epoll_ctl(MOD) on closed fd fails with EBADF.
    //    - break is hit -> evio_queue_fd_errors is called.
    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_invoke_pending(loop);

    assert_int_equal(counter, 1);
    assert_false(io2.active);

    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_enoent_add_fail)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    // 1. Start and run a watcher to add it to epoll.
    evio_poll io;
    evio_poll_init(&io, error_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);

    size_t counter = 0;
    io.data = &counter;

    evio_run(loop, EVIO_RUN_NOWAIT);

    // 2. Manually remove the FD from epoll to create a desync.
    struct epoll_event ev = { 0 };
    assert_int_equal(epoll_ctl(loop->fd, EPOLL_CTL_DEL, fds[0], &ev), 0);

    // 3. Close the fd and then queue a change.
    close(fds[0]);
    evio_poll_change(loop, &io, fds[0], EVIO_WRITE);

    // 4. Run. poll_update will:
    //    - op=MOD, epoll_ctl(MOD) fails with ENOENT.
    //    - case ENOENT:
    //    - epoll_ctl(ADD) on closed fd fails with EBADF.
    //    - break is hit -> evio_queue_fd_errors is called.
    evio_run(loop, EVIO_RUN_NOWAIT);
    evio_invoke_pending(loop);

    assert_int_equal(counter, 1);
    assert_false(io.active);

    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_poll_stale_event_invalidated)
{
    reset_cb_state();
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    evio_poll_start(loop, &io);
    evio_run(loop, EVIO_RUN_NOWAIT); // Add to epoll

    // Trigger an event so it's in the kernel's ready list.
    assert_int_equal(write(fds[1], "x", 1), 1);

    // Stop the watcher, which calls evio_invalidate_fd.
    evio_poll_stop(loop, &io);

    // Run the loop. It will process the DEL from the stop action.
    // It will also get the stale event. The handler will call evio_invalidate_fd,
    // which will now return 1 (already invalid), hitting the continue.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callback should have been called.
    assert_int_equal(generic_cb_called, 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}
