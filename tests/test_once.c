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

TEST(test_evio_once_by_poll)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_once once;
    evio_once_init(&once, generic_cb, fds[0], EVIO_READ);
    once.data = &data;
    evio_once_start(loop, &once, EVIO_TIME_FROM_SEC(10)); // Long timeout

    // Double start should be a no-op
    evio_once_start(loop, &once, EVIO_TIME_FROM_SEC(10));

    assert_int_equal(write(fds[1], "x", 1), 1);

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_ONCE);
    assert_true(data.emask & EVIO_READ);
    assert_int_equal(evio_refcount(loop), 0);

    // Double stop should be a no-op
    evio_once_stop(loop, &once);
    evio_once_stop(loop, &once);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_once_by_timer)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_once once;
    evio_once_init(&once, generic_cb, fds[0], EVIO_READ);
    once.data = &data;
    evio_once_start(loop, &once, 0); // Immediate timeout

    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 1);
    assert_true(data.emask & EVIO_ONCE);
    assert_true(data.emask & EVIO_TIMER);
    assert_int_equal(evio_refcount(loop), 0);

    // Double stop should be a no-op
    evio_once_stop(loop, &once);
    evio_once_stop(loop, &once);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_once_stop_with_pending)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    int fds[2] = { -1, -1 };
    assert_int_equal(pipe(fds), 0);

    evio_once once;
    evio_once_init(&once, generic_cb, fds[0], EVIO_READ);
    once.data = &data;
    evio_once_start(loop, &once, EVIO_TIME_FROM_SEC(10));

    // Manually feed events to all sub-watchers.
    evio_feed_event(loop, &once.base, EVIO_ONCE);
    evio_feed_event(loop, &once.io.base, EVIO_POLL);
    evio_feed_event(loop, &once.tm.base, EVIO_TIMER);
    assert_int_equal(evio_pending_count(loop), 3);

    // Stop it. This should clear all pending events.
    evio_once_stop(loop, &once);
    assert_int_equal(evio_pending_count(loop), 0);
    assert_int_equal(evio_refcount(loop), 0);

    // Running the loop should do nothing.
    evio_run(loop, EVIO_RUN_NOWAIT);
    assert_int_equal(data.called, 0);

    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}
