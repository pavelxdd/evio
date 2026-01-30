#include <stdlib.h>

#include <ev.h>
enum {
    LIBEV_READ  = EV_READ,
    LIBEV_WRITE = EV_WRITE,
};
#undef EV_READ
#undef EV_WRITE

#include <event2/event.h>
enum {
    LIBEVENT_READ  = EV_READ,
    LIBEVENT_WRITE = EV_WRITE,
};
#include <uv.h>

#include "evio.h"
#include "bench.h"

#define RUN_TIME_SEC 3

// --- evio ---
static void evio_idle_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *count = base->data;
    ++(*count);
}

static void evio_timeout_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_break(loop, EVIO_BREAK_ALL);
}

static void bench_evio_idle(bool use_uring)
{
    evio_loop *loop = evio_loop_new(use_uring ? EVIO_FLAG_URING : EVIO_FLAG_NONE);

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

    print_benchmark("idle_invocations", use_uring ? "evio-uring" : "evio", end - start, count);
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

// --- libevent ---
typedef struct {
    struct event_base *base;
    struct event *idle;
    size_t count;
} libevent_idle_ctx;

static void libevent_idle_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;

    libevent_idle_ctx *ctx = arg;
    ++ctx->count;

    struct timeval tv = { 0 };
    event_add(ctx->idle, &tv);
}

static void libevent_timeout_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;

    libevent_idle_ctx *ctx = arg;
    event_base_loopbreak(ctx->base);
}

static void bench_libevent_idle(void)
{
    libevent_idle_ctx ctx = {
        .base = event_base_new(),
    };
    if (!ctx.base) {
        abort();
    }

    ctx.idle = evtimer_new(ctx.base, libevent_idle_cb, &ctx);
    if (!ctx.idle) {
        abort();
    }
    struct timeval tv0 = { 0 };
    event_add(ctx.idle, &tv0);

    struct event *timeout = evtimer_new(ctx.base, libevent_timeout_cb, &ctx);
    if (!timeout) {
        abort();
    }
    struct timeval tv = { .tv_sec = RUN_TIME_SEC, .tv_usec = 0 };
    event_add(timeout, &tv);

    uint64_t start = get_time_ns();
    event_base_dispatch(ctx.base);
    uint64_t end = get_time_ns();

    print_benchmark("idle_invocations", "libevent", end - start, ctx.count);

    event_del(timeout);
    event_free(timeout);
    event_del(ctx.idle);
    event_free(ctx.idle);
    event_base_free(ctx.base);
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
    bench_evio_idle(false);
    bench_evio_idle(true);
    bench_libev_idle();
    bench_libevent_idle();
    bench_libuv_idle();
    return EXIT_SUCCESS;
}
