#include "test.h"

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

// Helper to mimic evio_poll_start's fd table management
static void prepare_fd_for_loop(evio_loop *loop, int fd)
{
    loop->fds.ptr = evio_list_resize(loop->fds.ptr, sizeof(*loop->fds.ptr),
                                     (size_t)fd + 1, &loop->fds.total);
    if ((size_t)fd >= loop->fds.count) {
        memset(&loop->fds.ptr[loop->fds.count], 0,
               (loop->fds.total - loop->fds.count) * sizeof(*loop->fds.ptr));
        loop->fds.count = (size_t)fd + 1;
    }
}

TEST(test_evio_clear_pending)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, generic_cb);
    prepare.data = &data;
    evio_prepare_start(loop, &prepare);

    assert_int_equal(evio_pending_count(loop), 0);

    // Queue an event manually
    evio_feed_event(loop, &prepare.base, EVIO_PREPARE);
    assert_int_equal(evio_pending_count(loop), 1);

    // Clear it
    evio_clear_pending(loop, &prepare.base);
    assert_int_equal(evio_pending_count(loop), 0);

    // Since the pending event was cleared, invoking pending events
    // should do nothing.
    evio_invoke_pending(loop);
    assert_int_equal(data.called, 0);

    // However, running the loop will trigger the prepare watcher normally.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

TEST(test_evio_core_clear_pending_middle)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare1;
    evio_prepare_init(&prepare1, generic_cb);
    prepare1.data = &data;
    evio_prepare_start(loop, &prepare1);

    evio_prepare prepare2;
    evio_prepare_init(&prepare2, generic_cb);
    prepare2.data = &data;
    evio_prepare_start(loop, &prepare2);

    evio_prepare prepare3;
    evio_prepare_init(&prepare3, generic_cb);
    prepare3.data = &data;
    evio_prepare_start(loop, &prepare3);

    evio_feed_event(loop, &prepare1.base, EVIO_PREPARE);
    evio_feed_event(loop, &prepare2.base, EVIO_PREPARE);
    evio_feed_event(loop, &prepare3.base, EVIO_PREPARE);
    assert_int_equal(evio_pending_count(loop), 3);

    // Clear the middle one. This will swap with the last one.
    evio_clear_pending(loop, &prepare2.base);
    assert_int_equal(evio_pending_count(loop), 2);

    evio_invoke_pending(loop);
    // prepare1 and prepare3's callbacks should be called.
    assert_int_equal(data.called, 2);

    evio_prepare_stop(loop, &prepare1);
    evio_prepare_stop(loop, &prepare2);
    evio_prepare_stop(loop, &prepare3);
    evio_loop_free(loop);
}

TEST(test_evio_core_requeue)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);

    evio_prepare prepare;
    evio_prepare_init(&prepare, generic_cb);
    prepare.data = &data;
    evio_prepare_start(loop, &prepare);

    // Queue event twice for same watcher before invoking
    evio_feed_event(loop, &prepare.base, EVIO_PREPARE);
    assert_int_equal(evio_pending_count(loop), 1);

    evio_feed_event(loop, &prepare.base, EVIO_PREPARE);
    assert_int_equal(evio_pending_count(loop), 1);

    evio_invoke_pending(loop);
    assert_int_equal(data.called, 1);

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

TEST(test_evio_feed_inactive_watcher)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, generic_cb);
    prepare.data = &data;
    // Watcher is initialized but not started, so it's inactive.

    evio_feed_event(loop, &prepare.base, EVIO_PREPARE);
    assert_int_equal(evio_pending_count(loop), 0);

    evio_invoke_pending(loop);
    assert_int_equal(data.called, 0);

    evio_loop_free(loop);
}

TEST(test_evio_feed_invalid_fd)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Call with invalid fds, should be no-ops and not crash.
    evio_feed_fd_event(loop, -1, EVIO_READ);
    evio_feed_fd_event(loop, 10000, EVIO_READ); // High fd, not in loop->fds

    evio_feed_fd_error(loop, -1);
    evio_feed_fd_error(loop, 10000);

    assert_int_equal(evio_pending_count(loop), 0);

    evio_loop_free(loop);
}

TEST(test_evio_queue_fd_events_mask_mismatch)
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

    // This call to evio_queue_fd_events will be for EVIO_WRITE.
    // The watcher is for EVIO_READ, so `w->emask & emask` will be false.
    evio_queue_fd_events(loop, fds[0], EVIO_WRITE);

    // No event should be queued.
    assert_int_equal(evio_pending_count(loop), 0);

    evio_invoke_pending(loop);
    assert_int_equal(data.called, 0);

    evio_poll_stop(loop, &io);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_queue_fd_errors_no_watchers)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);
    prepare_fd_for_loop(loop, fds[0]);

    // This fd is in loop->fds but has no watchers.
    // The for loop in evio_queue_fd_errors should not execute.
    evio_queue_fd_errors(loop, fds[0]);

    assert_int_equal(evio_pending_count(loop), 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_queue_fd_error_twice)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);
    prepare_fd_for_loop(loop, fds[0]);
    assert_int_equal(loop->fderrors.count, 0);

    // First call, should add an error.
    evio_queue_fd_error(loop, fds[0]);
    assert_int_equal(loop->fderrors.count, 1);
    assert_int_equal(loop->fds.ptr[fds[0]].errors, 1);

    // Second call, should be a no-op as `fds->errors` is already set.
    evio_queue_fd_error(loop, fds[0]);
    assert_int_equal(loop->fderrors.count, 1);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_queue_fd_change_twice)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);
    prepare_fd_for_loop(loop, fds[0]);
    assert_int_equal(loop->fdchanges.count, 0);

    // First call, should add a change.
    evio_queue_fd_change(loop, fds[0], EVIO_POLL);
    assert_int_equal(loop->fdchanges.count, 1);
    assert_int_equal(loop->fds.ptr[fds[0]].changes, 1);

    // Second call, should be a no-op for adding to the list,
    // as `fds->changes` is already set.
    evio_queue_fd_change(loop, fds[0], EVIO_POLL);
    assert_int_equal(loop->fdchanges.count, 1);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_invalidate_with_pending_change)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_poll io;
    evio_poll_init(&io, generic_cb, fds[0], EVIO_READ);
    io.data = &data;

    // Start watcher. This queues a change. fdchanges.count is 1.
    evio_poll_start(loop, &io);
    assert_int_equal(loop->fdchanges.count, 1);

    // Stop the watcher immediately, before the loop runs.
    // This will call evio_invalidate_fd, which will find the pending change
    // and call evio_flush_fd_change. Since there's only one change,
    // it will hit the `count <= 1` branch and just clear the list.
    evio_poll_stop(loop, &io);

    // After stopping, there should be no pending changes.
    assert_int_equal(loop->fdchanges.count, 0);
    assert_int_equal(evio_refcount(loop), 0);

    // Running the loop should do nothing.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_invalidate_with_pending_error)
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

    evio_run(loop, EVIO_RUN_NOWAIT); // Process changes so we can queue an error.

    // Queue an error. fderrors.count is 1.
    evio_queue_fd_error(loop, fds[0]);
    assert_int_equal(loop->fderrors.count, 1);

    // This will call evio_invalidate_fd, which will find the pending error
    // and call evio_flush_fd_error.
    evio_poll_stop(loop, &io);

    // After stopping, there should be no pending errors.
    assert_int_equal(loop->fderrors.count, 0);
    assert_int_equal(evio_refcount(loop), 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_flush_fd_change_multiple)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[3][2];
    evio_poll io[3];

    for (int i = 0; i < 3; ++i) {
        fds[i][0] = fds[i][1] = -1;
        assert_int_equal(pipe(fds[i]), 0);
        evio_poll_init(&io[i], generic_cb, fds[i][0], EVIO_READ);
        io[i].data = &data;
        evio_poll_start(loop, &io[i]);
    }
    assert_int_equal(loop->fdchanges.count, 3);
    assert_int_equal(loop->fds.ptr[fds[0][0]].changes, 1);
    assert_int_equal(loop->fds.ptr[fds[1][0]].changes, 2);
    assert_int_equal(loop->fds.ptr[fds[2][0]].changes, 3);

    // Stop the first watcher. This will call evio_invalidate_fd, then
    // evio_flush_fd_change. loop->fdchanges.count will be > 1, so the
    // 'else' branch should be taken.
    evio_poll_stop(loop, &io[0]);

    // The last element (fds[2][0]) should be moved to the flushed slot (index 0).
    assert_int_equal(loop->fdchanges.count, 2);
    assert_int_equal(loop->fdchanges.ptr[0], fds[2][0]);
    assert_int_equal(loop->fds.ptr[fds[2][0]].changes, 1);

    for (int i = 0; i < 3; ++i) {
        if (i != 0) {
            evio_poll_stop(loop, &io[i]);
        }
        close(fds[i][0]);
        close(fds[i][1]);
    }

    evio_loop_free(loop);
}

TEST(test_evio_flush_fd_error_multiple)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[3][2];
    evio_poll io[3];

    for (int i = 0; i < 3; ++i) {
        fds[i][0] = fds[i][1] = -1;
        assert_int_equal(pipe(fds[i]), 0);
        evio_poll_init(&io[i], generic_cb, fds[i][0], EVIO_READ);
        io[i].data = &data;
        evio_poll_start(loop, &io[i]);
    }
    evio_run(loop, EVIO_RUN_NOWAIT); // Process changes.

    for (int i = 0; i < 3; ++i) {
        evio_queue_fd_error(loop, fds[i][0]);
    }
    assert_int_equal(loop->fderrors.count, 3);
    assert_int_equal(loop->fds.ptr[fds[0][0]].errors, 1);
    assert_int_equal(loop->fds.ptr[fds[1][0]].errors, 2);
    assert_int_equal(loop->fds.ptr[fds[2][0]].errors, 3);

    // Stop the first watcher. This will call evio_invalidate_fd, then
    // evio_flush_fd_error. loop->fderrors.count will be > 1, so the
    // 'else' branch should be taken.
    evio_poll_stop(loop, &io[0]);

    // The last element (fds[2][0]) should be moved to the flushed slot (index 0).
    assert_int_equal(loop->fderrors.count, 2);
    assert_int_equal(loop->fderrors.ptr[0], fds[2][0]);
    assert_int_equal(loop->fds.ptr[fds[2][0]].errors, 1);

    for (int i = 0; i < 3; ++i) {
        if (i != 0) {
            evio_poll_stop(loop, &io[i]);
        }
        close(fds[i][0]);
        close(fds[i][1]);
    }

    evio_loop_free(loop);
}

TEST(test_evio_invalidate_fd_ebadf)
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
    evio_run(loop, EVIO_RUN_NOWAIT); // Process ADD to epoll.

    // Close the fd while it is still being watched.
    close(fds[0]);

    // Stopping the watcher will now call evio_invalidate_fd, which will try
    // to DEL the closed fd from epoll, resulting in EBADF. This covers
    // the `errno != EPERM` path in evio_invalidate_fd.
    evio_poll_stop(loop, &io);

    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_invalidate_fd_eperm)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fd = open("/dev/null", O_RDONLY);
    assert_true(fd >= 0);

    // Manually ensure fd is in range and initialized.
    prepare_fd_for_loop(loop, fd);

    // epoll_ctl(DEL) on /dev/null gives EPERM. evio_invalidate_fd should
    // treat this as success (return 0).
    assert_int_equal(evio_invalidate_fd(loop, fd), 0);

    close(fd);
    evio_loop_free(loop);
}

TEST(test_evio_core_invalidate_twice)
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

    // Run once to get the fd added to epoll
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    // Trigger an event, so it's in epoll's ready list
    assert_int_equal(write(fds[1], "x", 1), 1);

    // Stop the watcher. This calls evio_invalidate_fd, which sets the
    // EVIO_FD_INVAL flag and calls epoll_ctl(DEL).
    evio_poll_stop(loop, &io);
    assert_int_equal(evio_refcount(loop), 0);
    assert_false(io.active);

    // Now run the loop. epoll_pwait may still return the event that was
    // ready before the DEL. evio_poll_wait will then call evio_invalidate_fd
    // a second time. Inside this call, fds->list.count will be 0 and
    // fds->flags will have EVIO_FD_INVAL set, which covers the target line.
    evio_run(loop, EVIO_RUN_NOWAIT);

    // No callback should be called because we stopped the watcher.
    assert_int_equal(data.called, 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_core_invalidate_twice_manual)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);
    int fd = fds[0];

    // Manually add fd to loop->fds.
    prepare_fd_for_loop(loop, fd);

    // First invalidation. This will set the EVIO_FD_INVAL flag.
    // The return value depends on whether epoll_ctl succeeds.
    evio_invalidate_fd(loop, fd);

    // Second invalidation, this should hit the `if (fds->flags & EVIO_FD_INVAL)` branch.
    assert_int_equal(evio_invalidate_fd(loop, fd), 1);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_prepare_fd_for_loop_coverage)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Use file descriptors that are guaranteed to be different and ordered
    int fd_small = open("/dev/null", O_RDONLY);
    assert_true(fd_small >= 0);
    int fd_large = open("/dev/null", O_RDONLY);
    assert_true(fd_large > fd_small);

    // This call covers the `if` condition being true
    prepare_fd_for_loop(loop, fd_large);
    assert_int_equal(loop->fds.count, fd_large + 1);

    // This call covers the `if` condition being false
    prepare_fd_for_loop(loop, fd_small);
    assert_int_equal(loop->fds.count, fd_large + 1); // should not change

    close(fd_small);
    close(fd_large);
    evio_loop_free(loop);
}

// Test boundary checks on queue/invalidate functions
TEST(test_evio_core_asserts_boundaries)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    // Test with invalid fd (-1)
    expect_assert_failure(evio_queue_fd_events(loop, -1, EVIO_READ));
    expect_assert_failure(evio_queue_fd_errors(loop, -1));
    expect_assert_failure(evio_queue_fd_error(loop, -1));
    expect_assert_failure(evio_queue_fd_change(loop, -1, 0));
    expect_assert_failure(evio_flush_fd_change(loop, -1));
    expect_assert_failure(evio_flush_fd_error(loop, -1));
    expect_assert_failure(evio_invalidate_fd(loop, -1));

    // Test with out-of-bounds fd (loop->fds.count is 0)
    expect_assert_failure(evio_queue_fd_events(loop, 1, EVIO_READ));
    expect_assert_failure(evio_queue_fd_errors(loop, 1));
    expect_assert_failure(evio_queue_fd_error(loop, 1));
    expect_assert_failure(evio_queue_fd_change(loop, 1, 0));
    expect_assert_failure(evio_flush_fd_change(loop, 1));
    expect_assert_failure(evio_flush_fd_error(loop, 1));
    expect_assert_failure(evio_invalidate_fd(loop, 1));

    evio_loop_free(loop);
}

TEST(test_evio_core_asserts_consistency)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, dummy_cb);
    evio_prepare_start(loop, &prepare);

    // Test evio_queue_event consistency asserts
    evio_feed_event(loop, &prepare.base, EVIO_PREPARE);
    assert_true(prepare.base.pending > 0);
    size_t original_pending = prepare.base.pending;
    prepare.base.pending = 999; // Corrupt pending state
    expect_assert_failure(evio_queue_event(loop, &prepare.base, EVIO_PREPARE));
    prepare.base.pending = original_pending;
    evio_clear_pending(loop, &prepare.base);

    // Test evio_clear_pending consistency asserts
    evio_feed_event(loop, &prepare.base, EVIO_PREPARE);
    assert_true(prepare.base.pending > 0);
    original_pending = prepare.base.pending;
    prepare.base.pending = 999; // Corrupt pending state
    expect_assert_failure(evio_clear_pending(loop, &prepare.base));
    prepare.base.pending = original_pending;

    evio_pending_list *pending = &loop->pending[(prepare.base.pending - 1) & 1];
    size_t index = (prepare.base.pending - 1) >> 1;
    evio_base *original_base = pending->ptr[index].base;
    pending->ptr[index].base = NULL; // Corrupt base pointer in pending queue
    expect_assert_failure(evio_clear_pending(loop, &prepare.base));
    pending->ptr[index].base = original_base;
    evio_clear_pending(loop, &prepare.base);

    evio_prepare_stop(loop, &prepare);
    evio_loop_free(loop);
}

TEST(test_evio_core_asserts_invoke)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_prepare prepare;
    evio_prepare_init(&prepare, dummy_cb);
    evio_prepare_start(loop, &prepare);

    // Test evio_invoke_pending consistency assert
    evio_feed_event(loop, &prepare.base, EVIO_PREPARE);
    assert_true(prepare.base.pending > 0);
    prepare.base.pending = 999;
    expect_assert_failure(evio_invoke_pending(loop));

    evio_loop_free(loop);
}

TEST(test_evio_core_asserts_flush_fd_change_fd_out_of_bounds)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    prepare_fd_for_loop(loop, 10);
    evio_queue_fd_change(loop, 5, EVIO_POLL);  // idx 0
    evio_queue_fd_change(loop, 10, EVIO_POLL); // idx 1
    assert_int_equal(loop->fdchanges.count, 2);

    // Corrupt last element to be out-of-bounds fd
    loop->fdchanges.ptr[1] = 99; // fd 99 > loop->fds.count
    expect_assert_failure(evio_flush_fd_change(loop, 0));

    loop->fdchanges.ptr[1] = -1; // fd < 0
    expect_assert_failure(evio_flush_fd_change(loop, 0));

    evio_loop_free(loop);
}

TEST(test_evio_core_asserts_flush_fd_change_backptr)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    prepare_fd_for_loop(loop, 10);
    evio_queue_fd_change(loop, 5, EVIO_POLL);  // idx 0
    evio_queue_fd_change(loop, 10, EVIO_POLL); // idx 1
    assert_int_equal(loop->fdchanges.count, 2);

    // Corrupt back-pointer for last element
    loop->fds.ptr[10].changes = 99; // should be 2
    expect_assert_failure(evio_flush_fd_change(loop, 0));

    evio_loop_free(loop);
}

TEST(test_evio_core_asserts_flush_fd_error_fd_out_of_bounds)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    prepare_fd_for_loop(loop, 10);
    evio_queue_fd_error(loop, 5);  // idx 0
    evio_queue_fd_error(loop, 10); // idx 1
    assert_int_equal(loop->fderrors.count, 2);

    // Corrupt last element to be out-of-bounds fd
    loop->fderrors.ptr[1] = 99; // fd 99 > loop->fds.count
    expect_assert_failure(evio_flush_fd_error(loop, 0));

    loop->fderrors.ptr[1] = -1; // fd < 0
    expect_assert_failure(evio_flush_fd_error(loop, 0));

    evio_loop_free(loop);
}

TEST(test_evio_core_asserts_flush_fd_error_backptr)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    prepare_fd_for_loop(loop, 10);
    evio_queue_fd_error(loop, 5);  // idx 0
    evio_queue_fd_error(loop, 10); // idx 1
    assert_int_equal(loop->fderrors.count, 2);

    // Corrupt back-pointer for last element
    loop->fds.ptr[10].errors = 99; // should be 2
    expect_assert_failure(evio_flush_fd_error(loop, 0));

    evio_loop_free(loop);
}
