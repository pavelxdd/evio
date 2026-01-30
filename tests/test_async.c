#include "test.h"

#include <sys/eventfd.h>

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

    // Double start: no-op
    evio_async_start(loop, &async);

    pthread_t thread;
    thread_arg arg = { .loop = loop, .async = &async };
    assert_int_equal(pthread_create(&thread, NULL, thread_func, &arg), 0);

    evio_run(loop, EVIO_RUN_ONCE);

    assert_int_equal(data.called, 1);
    assert_int_equal(data.emask, EVIO_ASYNC);

    assert_int_equal(pthread_join(thread, NULL), 0);

    evio_async_stop(loop, &async);
    // Double stop: no-op
    evio_async_stop(loop, &async);
    evio_loop_free(loop);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    size_t called;
} async_wait;

static bool async_wait_for(async_wait *w, size_t want, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    ts.tv_nsec += (long)timeout_ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;

    pthread_mutex_lock(&w->mu);
    while (w->called < want) {
        int rc = pthread_cond_timedwait(&w->cv, &w->mu, &ts);
        if (rc == ETIMEDOUT) {
            break;
        }
        assert_int_equal(rc, 0);
    }
    bool ok = w->called >= want;
    pthread_mutex_unlock(&w->mu);
    return ok;
}

TEST(test_async_wait_for_timeout)
{
    async_wait w = { 0 };
    pthread_mutex_init(&w.mu, NULL);
    pthread_cond_init(&w.cv, NULL);

    assert_false(async_wait_for(&w, 1, 0));

    pthread_cond_destroy(&w.cv);
    pthread_mutex_destroy(&w.mu);
}

static void async_wait_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    (void)loop;
    assert_true(emask & EVIO_ASYNC);

    async_wait *w = base->data;

    pthread_mutex_lock(&w->mu);
    ++w->called;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

static void breaker_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    (void)emask;

    evio_poll *w = container_of(base, evio_poll, base);

    eventfd_t val;
    (void)read(w->fd, &val, sizeof(val));

    evio_break(loop, EVIO_BREAK_ALL);
}

typedef struct {
    evio_loop *loop;
} run_arg;

static void *run_loop_thread(void *ptr)
{
    run_arg *arg = ptr;
    evio_run(arg->loop, EVIO_RUN_DEFAULT);
    return NULL;
}

TEST(test_evio_async_double_send_wakes_loop_twice)
{
    async_wait w = { 0 };
    pthread_mutex_init(&w.mu, NULL);
    pthread_cond_init(&w.cv, NULL);

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(loop);

    evio_async async;
    evio_async_init(&async, async_wait_cb);
    async.data = &w;
    evio_async_start(loop, &async);

    int breaker_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert_true(breaker_fd >= 0);

    evio_poll breaker;
    evio_poll_init(&breaker, breaker_cb, breaker_fd, EVIO_READ);
    evio_poll_start(loop, &breaker);

    pthread_t thread;
    run_arg arg = { .loop = loop };
    assert_int_equal(pthread_create(&thread, NULL, run_loop_thread, &arg), 0);

    for (int i = 1000; i--;) {
        if (atomic_load_explicit(&loop->eventfd_allow.value, memory_order_acquire)) {
            break;
        }
        usleep(100);
    }

    evio_async_send(loop, &async);
    assert_true(async_wait_for(&w, 1, 200));

    bool saw_disallow = false;
    for (int i = 1000; i--;) {
        if (!atomic_load_explicit(&loop->eventfd_allow.value, memory_order_acquire)) {
            saw_disallow = true; // GCOVR_EXCL_LINE
        } else if (saw_disallow) {
            break; // GCOVR_EXCL_LINE
        }
        usleep(100);
    }

    evio_async_send(loop, &async);

    bool woke_twice = async_wait_for(&w, 2, 50);

    eventfd_t one = 1;
    (void)write(breaker_fd, &one, sizeof(one));

    assert_int_equal(pthread_join(thread, NULL), 0);

    evio_poll_stop(loop, &breaker);
    close(breaker_fd);

    evio_async_stop(loop, &async);
    evio_loop_free(loop);

    pthread_cond_destroy(&w.cv);
    pthread_mutex_destroy(&w.mu);

    assert_true(woke_twice);
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

    // Both threads sent; only one triggers evio_eventfd_write.
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
