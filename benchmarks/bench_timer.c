#include <stdlib.h>

#include <ev.h>
#include <uv.h>

#include "evio.h"
#include "bench.h"

// --- Common dummy callbacks for overhead benchmark ---
static void dummy_evio_cb(evio_loop *loop, evio_base *base, evio_mask emask) {}
static void dummy_libev_cb(struct ev_loop *loop, ev_timer *w, int revents) {}
// For libuv, the callback is optional in uv_timer_start.

// =============================================================================
//   Timer Overhead Benchmark
// =============================================================================
// This benchmark measures the overhead of starting and stopping a timer watcher
// in a tight loop. It does not involve running the event loop to fire the
// timer, focusing solely on the cost of managing the library's internal timer
// data structures (e.g., adding to and removing from a min-heap).
//
#define NUM_OVERHEAD_ITERATIONS 1000000

// --- evio ---
static void bench_evio_timer_overhead(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);

    evio_timer timer;
    evio_timer_init(&timer, dummy_evio_cb, 0);

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_OVERHEAD_ITERATIONS; ++i) {
        evio_timer_start(loop, &timer, EVIO_TIME_FROM_SEC(1));
        evio_timer_stop(loop, &timer);
    }
    uint64_t end = get_time_ns();

    print_benchmark("timer_overhead", "evio", end - start, NUM_OVERHEAD_ITERATIONS);
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

// =============================================================================
//   Many Active Timers Benchmark
// =============================================================================
// This benchmark measures how quickly the event loop can process a "storm" of
// timers that are all due to fire at the same time. A large number of timers
// are started with a zero timeout, making them all ready for immediate
// processing in the first loop iteration.
//
// This tests the efficiency of:
// 1. Iterating through and removing many expired timers from the heap.
// 2. Queueing and dispatching the corresponding callbacks.
//
#define NUM_MANY_TIMERS 50000

// --- evio ---
static void evio_many_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *count = base->data;
    if (++(*count) == NUM_MANY_TIMERS) {
        evio_break(loop, EVIO_BREAK_ALL);
    }
}

static void bench_evio_timer_many_active(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);
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

    print_benchmark("timer_many_active", "evio", end - start, NUM_MANY_TIMERS);

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

int main(void)
{
    print_versions();

    bench_evio_timer_overhead();
    bench_libev_timer_overhead();
    bench_libuv_timer_overhead();

    printf("\n");

    bench_evio_timer_many_active();
    bench_libev_timer_many_active();
    bench_libuv_timer_many_active();

    return EXIT_SUCCESS;
}
