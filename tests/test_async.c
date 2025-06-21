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

typedef struct {
    evio_loop *loop;
    evio_async *async;
} thread_arg;

static void *thread_func(void *ptr)
{
    thread_arg *arg = ptr;
    evio_async_send(arg->loop, arg->async);
    return NULL;
}

TEST(test_evio_async)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_async async;
    evio_async_init(&async, generic_cb);
    async.data = &data;
    evio_async_start(loop, &async);

    // Double start should be a no-op
    evio_async_start(loop, &async);

    pthread_t thread;
    thread_arg arg = { .loop = loop, .async = &async };
    assert_int_equal(pthread_create(&thread, NULL, thread_func, &arg), 0);

    // This will block until the eventfd is written to by the other thread
    evio_run(loop, EVIO_RUN_ONCE);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_ASYNC);

    assert_int_equal(pthread_join(thread, NULL), 0);

    evio_async_stop(loop, &async);
    // Double stop should be a no-op
    evio_async_stop(loop, &async);
    evio_loop_free(loop);
}

typedef struct {
    evio_loop *loop;
    evio_async *async;
    pthread_barrier_t *barrier;
} thread_arg_multi;

static void *thread_func_multi(void *arg)
{
    thread_arg_multi *t_arg = arg;
    pthread_barrier_wait(t_arg->barrier);
    evio_async_send(t_arg->loop, t_arg->async);
    return NULL;
}

TEST(test_evio_async_multi_send)
{
    generic_cb_data data = { 0 };
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_async async;
    evio_async_init(&async, generic_cb);
    async.data = &data;
    evio_async_start(loop, &async);

    pthread_t threads[2];
    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 3);

    thread_arg_multi arg = { .loop = loop, .async = &async, .barrier = &barrier };

    assert_int_equal(pthread_create(&threads[0], NULL, thread_func_multi, &arg), 0);
    assert_int_equal(pthread_create(&threads[1], NULL, thread_func_multi, &arg), 0);

    // Wait for threads to be ready, then unblock them.
    pthread_barrier_wait(&barrier);

    assert_int_equal(pthread_join(threads[0], NULL), 0);
    assert_int_equal(pthread_join(threads[1], NULL), 0);

    // Both threads sent, but only one should have triggered evio_eventfd_write.
    // We expect one callback.
    evio_run(loop, EVIO_RUN_NOWAIT);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_ASYNC);

    evio_async_stop(loop, &async);
    evio_loop_free(loop);
    pthread_barrier_destroy(&barrier);
}

TEST(test_evio_async_pending)
{
    evio_async async;
    evio_async_init(&async, generic_cb);

    generic_cb_data data = { 0 };
    async.data = &data;

    assert_false(evio_async_pending(&async));

    // Simulate send
    atomic_store_explicit(&async.status.value, 1, memory_order_release);
    assert_true(evio_async_pending(&async));

    assert_int_equal(data.called, 0);
}
