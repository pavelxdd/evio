#include <stdlib.h>

#include <ev.h>
#include <uv.h>

#include "evio.h"
#include "bench.h"

#define RUN_TIME_SEC 3

// --- evio ---
static void evio_idle_cb(evio_loop *loop, evio_base *w, evio_mask emask)
{
    size_t *count = w->data;
    ++(*count);
}

static void evio_timeout_cb(evio_loop *loop, evio_base *w, evio_mask emask)
{
    evio_break(loop, EVIO_BREAK_ALL);
}

static void bench_evio_idle(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);

    evio_idle idle;
    evio_idle_init(&idle, evio_idle_cb);
    evio_idle_start(loop, &idle);

    evio_timer timer;
    evio_timer_init(&timer, evio_timeout_cb, 0);
    evio_timer_start(loop, &timer, EVIO_TIME_FROM_SEC(RUN_TIME_SEC));

    size_t count = 0;
    idle.data = &count;

    uint64_t start = get_time_ns();
    evio_run(loop, EVIO_RUN_DEFAULT);
    uint64_t end = get_time_ns();

    print_benchmark("idle_invocations", "evio", end - start, count);
    evio_loop_free(loop);
}

// --- libev ---
static void libev_idle_cb(struct ev_loop *loop, ev_idle *w, int revents)
{
    size_t *count = w->data;
    ++(*count);
}

static void libev_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents)
{
    ev_break(loop, EVBREAK_ALL);
}

static void bench_libev_idle(void)
{
    struct ev_loop *loop = ev_loop_new(0);

    ev_idle idle;
    ev_idle_init(&idle, libev_idle_cb);
    ev_idle_start(loop, &idle);

    ev_timer timeout;
    ev_timer_init(&timeout, libev_timeout_cb, RUN_TIME_SEC, 0);
    ev_timer_start(loop, &timeout);

    size_t count = 0;
    idle.data = &count;

    uint64_t start = get_time_ns();
    ev_run(loop, 0);
    uint64_t end = get_time_ns();

    print_benchmark("idle_invocations", "libev", end - start, count);
    ev_loop_destroy(loop);
}

// --- libuv ---
static void libuv_idle_cb(uv_idle_t *handle)
{
    size_t *count = handle->data;
    ++(*count);
}

static void libuv_timeout_cb(uv_timer_t *handle)
{
    uv_stop(handle->loop);
}

static void bench_libuv_idle(void)
{
    uv_loop_t *loop = uv_loop_new();

    uv_idle_t idle;
    uv_idle_init(loop, &idle);
    uv_idle_start(&idle, libuv_idle_cb);

    uv_timer_t timeout;
    uv_timer_init(loop, &timeout);
    uv_timer_start(&timeout, libuv_timeout_cb, RUN_TIME_SEC * 1000, 0);

    size_t count = 0;
    idle.data = &count;

    uint64_t start = get_time_ns();
    uv_run(loop, UV_RUN_DEFAULT);
    uint64_t end = get_time_ns();

    print_benchmark("idle_invocations", "libuv", end - start, count);

    uv_close((uv_handle_t *)&idle, NULL);
    uv_close((uv_handle_t *)&timeout, NULL);
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    free(loop);
}

int main(void)
{
    print_versions();
    bench_evio_idle();
    bench_libev_idle();
    bench_libuv_idle();
    return EXIT_SUCCESS;
}
