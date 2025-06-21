#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <ev.h>
#include <uv.h>

#include "evio.h"
#include "bench.h"

#define NUM_WATCHERS 256
#define NUM_ITERATIONS 10000

// --- Common dummy callbacks ---
static void dummy_evio_cb(evio_loop *loop, evio_base *base, evio_mask emask) {}
static void dummy_libev_cb(struct ev_loop *loop, ev_io *w, int revents) {}
static void dummy_libuv_cb(uv_poll_t *w, int status, int events) {}

// --- evio ---
static void bench_evio_churn(bool use_uring)
{
    evio_loop *loop = evio_loop_new(use_uring ? EVIO_FLAG_URING : EVIO_FLAG_NONE);

    evio_poll watchers[NUM_WATCHERS];
    int fds[NUM_WATCHERS];

    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        fds[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        evio_poll_init(&watchers[i], dummy_evio_cb, fds[i], EVIO_WRITE);
    }

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            evio_poll_start(loop, &watchers[j]);
        }
        evio_run(loop, EVIO_RUN_NOWAIT);

        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            evio_poll_stop(loop, &watchers[j]);
        }
        evio_run(loop, EVIO_RUN_NOWAIT);
    }
    uint64_t end = get_time_ns();

    print_benchmark("poll_churn", use_uring ? "evio-uring" : "evio", end - start, NUM_ITERATIONS * NUM_WATCHERS);

    evio_loop_free(loop);
    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        close(fds[i]);
    }
}

// --- libev ---
static void bench_libev_churn(void)
{
    struct ev_loop *loop = ev_loop_new(0);

    ev_io watchers[NUM_WATCHERS];
    int fds[NUM_WATCHERS];

    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        fds[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        ev_io_init(&watchers[i], dummy_libev_cb, fds[i], EV_WRITE);
    }

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            ev_io_start(loop, &watchers[j]);
        }
        ev_run(loop, EVRUN_NOWAIT);

        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            ev_io_stop(loop, &watchers[j]);
        }
        ev_run(loop, EVRUN_NOWAIT);
    }
    uint64_t end = get_time_ns();

    print_benchmark("poll_churn", "libev", end - start, NUM_ITERATIONS * NUM_WATCHERS);

    ev_loop_destroy(loop);
    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        close(fds[i]);
    }
}

// --- libuv ---
static void bench_libuv_churn(void)
{
    uv_loop_t *loop = uv_loop_new();

    uv_poll_t watchers[NUM_WATCHERS];
    int fds[NUM_WATCHERS];

    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        fds[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        uv_poll_init(loop, &watchers[i], fds[i]);
    }

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            uv_poll_start(&watchers[j], UV_WRITABLE, dummy_libuv_cb);
        }
        uv_run(loop, UV_RUN_NOWAIT);

        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            uv_poll_stop(&watchers[j]);
        }
        uv_run(loop, UV_RUN_NOWAIT);
    }
    uint64_t end = get_time_ns();

    print_benchmark("poll_churn", "libuv", end - start, NUM_ITERATIONS * NUM_WATCHERS);

    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        if (!uv_is_closing((uv_handle_t *)&watchers[i])) {
            uv_close((uv_handle_t *)&watchers[i], NULL);
        }
    }
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    free(loop);
    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        close(fds[i]);
    }
}

int main(void)
{
    print_versions();
    bench_evio_churn(false);
    bench_evio_churn(true);
    bench_libev_churn();
    bench_libuv_churn();
    return EXIT_SUCCESS;
}
