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

// Dummy callbacks (overhead benchmark).
static void dummy_evio_cb(evio_loop *loop, evio_base *base, evio_mask emask) {}
static void dummy_libev_cb(struct ev_loop *loop, ev_timer *w, int revents) {}
// libuv: callback may be NULL in uv_timer_start().

// Timer start/stop overhead (no loop run).
#define NUM_OVERHEAD_ITERATIONS 1000000

// --- evio ---
static void bench_evio_timer_overhead(bool use_uring)
{
    evio_loop *loop = evio_loop_new(use_uring ? EVIO_FLAG_URING : EVIO_FLAG_NONE);

    evio_timer timer;
    evio_timer_init(&timer, dummy_evio_cb, 0);

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_OVERHEAD_ITERATIONS; ++i) {
        evio_timer_start(loop, &timer, EVIO_TIME_FROM_SEC(1));
        evio_timer_stop(loop, &timer);
    }
    uint64_t end = get_time_ns();

    print_benchmark("timer_overhead", use_uring ? "evio-uring" : "evio",
                    end - start, NUM_OVERHEAD_ITERATIONS);
    evio_loop_free(loop);
}

// --- libev ---
static void bench_libev_timer_overhead(void)
{
    struct ev_loop *loop = ev_loop_new(0);

    ev_timer timer;
    ev_timer_init(&timer, dummy_libev_cb, 1.0, 0.0);

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_OVERHEAD_ITERATIONS; ++i) {
        ev_timer_start(loop, &timer);
        ev_timer_stop(loop, &timer);
    }
    uint64_t end = get_time_ns();

    print_benchmark("timer_overhead", "libev", end - start, NUM_OVERHEAD_ITERATIONS);
    ev_loop_destroy(loop);
}

// --- libevent ---
static void dummy_libevent_cb(evutil_socket_t fd, short what, void *arg) {}

static void bench_libevent_timer_overhead(void)
{
    struct event_base *base = event_base_new();
    if (!base) {
        abort();
    }

    struct event *timer = evtimer_new(base, dummy_libevent_cb, NULL);
    if (!timer) {
        abort();
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_OVERHEAD_ITERATIONS; ++i) {
        event_add(timer, &tv);
        event_del(timer);
    }
    uint64_t end = get_time_ns();

    print_benchmark("timer_overhead", "libevent", end - start, NUM_OVERHEAD_ITERATIONS);

    event_free(timer);
    event_base_free(base);
}

// --- libuv ---
static void bench_libuv_timer_overhead(void)
{
    uv_loop_t *loop = uv_loop_new();

    uv_timer_t timer;
    uv_timer_init(loop, &timer);

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_OVERHEAD_ITERATIONS; ++i) {
        uv_timer_start(&timer, NULL, 1000, 0);
        uv_timer_stop(&timer);
    }
    uint64_t end = get_time_ns();

    print_benchmark("timer_overhead", "libuv", end - start, NUM_OVERHEAD_ITERATIONS);

    uv_close((uv_handle_t *)&timer, NULL);
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    free(loop);
}

// Many timers due at t=0.
#define NUM_MANY_TIMERS 50000

// --- evio ---
static void evio_many_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *count = base->data;
    if (++(*count) == NUM_MANY_TIMERS) {
        evio_break(loop, EVIO_BREAK_ALL);
    }
}

static void bench_evio_timer_many_active(bool use_uring)
{
    evio_loop *loop = evio_loop_new(use_uring ? EVIO_FLAG_URING : EVIO_FLAG_NONE);
    evio_timer *timers = evio_calloc(NUM_MANY_TIMERS, sizeof(evio_timer));

    size_t count = 0;

    for (size_t i = 0; i < NUM_MANY_TIMERS; ++i) {
        evio_timer_init(&timers[i], evio_many_cb, 0);
        evio_timer_start(loop, &timers[i], 0);
        timers[i].data = &count;
    }

    uint64_t start = get_time_ns();
    evio_run(loop, EVIO_RUN_DEFAULT);
    uint64_t end = get_time_ns();

    print_benchmark("timer_many_active", use_uring ? "evio-uring" : "evio",
                    end - start, NUM_MANY_TIMERS);

    evio_free(timers);
    evio_loop_free(loop);
}

// --- libev ---
static void libev_many_cb(struct ev_loop *loop, ev_timer *w, int revents)
{
    size_t *count = w->data;
    if (++(*count) == NUM_MANY_TIMERS) {
        ev_break(loop, EVBREAK_ALL);
    }
}

static void bench_libev_timer_many_active(void)
{
    struct ev_loop *loop = ev_loop_new(0);
    ev_timer *timers = calloc(NUM_MANY_TIMERS, sizeof(ev_timer));

    size_t count = 0;

    for (size_t i = 0; i < NUM_MANY_TIMERS; ++i) {
        ev_timer_init(&timers[i], libev_many_cb, 0., 0.);
        ev_timer_start(loop, &timers[i]);
        timers[i].data = &count;
    }

    uint64_t start = get_time_ns();
    ev_run(loop, 0);
    uint64_t end = get_time_ns();

    print_benchmark("timer_many_active", "libev", end - start, NUM_MANY_TIMERS);

    free(timers);
    ev_loop_destroy(loop);
}

// --- libuv ---
static void libuv_many_cb(uv_timer_t *handle)
{
    size_t *count = handle->data;
    if (++(*count) == NUM_MANY_TIMERS) {
        uv_stop(handle->loop);
    }
}

static void bench_libuv_timer_many_active(void)
{
    uv_loop_t *loop = uv_loop_new();
    uv_timer_t *timers = calloc(NUM_MANY_TIMERS, sizeof(uv_timer_t));

    size_t count = 0;

    for (size_t i = 0; i < NUM_MANY_TIMERS; ++i) {
        uv_timer_init(loop, &timers[i]);
        uv_timer_start(&timers[i], libuv_many_cb, 0, 0);
        timers[i].data = &count;
    }

    uint64_t start = get_time_ns();
    uv_run(loop, UV_RUN_DEFAULT);
    uint64_t end = get_time_ns();

    print_benchmark("timer_many_active", "libuv", end - start, NUM_MANY_TIMERS);

    for (size_t i = 0; i < NUM_MANY_TIMERS; ++i) {
        if (!uv_is_closing((uv_handle_t *)&timers[i])) {
            uv_close((uv_handle_t *)&timers[i], NULL);
        }
    }
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    free(loop);
    free(timers);
}

// --- libevent ---
static void libevent_many_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;

    struct {
        struct event_base *base;
        size_t *count;
    } *ctx = arg;

    if (++(*ctx->count) == NUM_MANY_TIMERS) {
        event_base_loopbreak(ctx->base);
    }
}

static void bench_libevent_timer_many_active(void)
{
    struct event_base *base = event_base_new();
    if (!base) {
        abort();
    }
    struct event **timers = calloc(NUM_MANY_TIMERS, sizeof(*timers));
    if (!timers) {
        abort();
    }

    size_t count = 0;
    struct {
        struct event_base *base;
        size_t *count;
    } ctx = { .base = base, .count = &count };

    struct timeval tv = { 0 };
    for (size_t i = 0; i < NUM_MANY_TIMERS; ++i) {
        timers[i] = evtimer_new(base, libevent_many_cb, &ctx);
        if (!timers[i]) {
            abort();
        }
        event_add(timers[i], &tv);
    }

    uint64_t start = get_time_ns();
    event_base_dispatch(base);
    uint64_t end = get_time_ns();

    print_benchmark("timer_many_active", "libevent", end - start, NUM_MANY_TIMERS);

    for (size_t i = 0; i < NUM_MANY_TIMERS; ++i) {
        event_free(timers[i]);
    }
    free(timers);
    event_base_free(base);
}

int main(void)
{
    print_versions();

    bench_evio_timer_overhead(false);
    bench_evio_timer_overhead(true);
    bench_libev_timer_overhead();
    bench_libevent_timer_overhead();
    bench_libuv_timer_overhead();

    printf("\n");

    bench_evio_timer_many_active(false);
    bench_evio_timer_many_active(true);
    bench_libev_timer_many_active();
    bench_libevent_timer_many_active();
    bench_libuv_timer_many_active();

    return EXIT_SUCCESS;
}
