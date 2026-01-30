#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

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

#define NUM_WATCHERS 256
#define NUM_ITERATIONS 10000

// --- Common dummy callbacks ---
static void dummy_evio_cb(evio_loop *loop, evio_base *base, evio_mask emask) {}
static void dummy_libev_cb(struct ev_loop *loop, ev_io *w, int revents) {}
static void dummy_libuv_cb(uv_poll_t *w, int status, int events) {}
static void dummy_libevent_cb(evutil_socket_t fd, short what, void *arg) {}

// --- evio ---
static void bench_evio_churn(int fds[NUM_WATCHERS], bool use_uring)
{
    evio_loop *loop = evio_loop_new(use_uring ? EVIO_FLAG_URING : EVIO_FLAG_NONE);
    evio_poll io[NUM_WATCHERS];

    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        evio_poll_init(&io[i], dummy_evio_cb, fds[i], EVIO_READ | EVIO_WRITE);
    }

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            evio_poll_start(loop, &io[j]);
        }
        evio_run(loop, EVIO_RUN_NOWAIT);

        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            evio_poll_stop(loop, &io[j]);
        }
        evio_run(loop, EVIO_RUN_NOWAIT);
    }
    uint64_t end = get_time_ns();

    const char *name = use_uring ? "evio-uring" : "evio";
    print_benchmark("poll_churn", name, end - start, NUM_ITERATIONS * NUM_WATCHERS * 2);

    evio_loop_free(loop);
}

// --- libev ---
static void bench_libev_churn(int fds[NUM_WATCHERS])
{
    struct ev_loop *loop = ev_loop_new(0);
    ev_io io[NUM_WATCHERS];

    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        ev_io_init(&io[i], dummy_libev_cb, fds[i], LIBEV_READ | LIBEV_WRITE);
    }

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            ev_io_start(loop, &io[j]);
        }
        ev_run(loop, EVRUN_NOWAIT);

        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            ev_io_stop(loop, &io[j]);
        }
        ev_run(loop, EVRUN_NOWAIT);
    }
    uint64_t end = get_time_ns();

    print_benchmark("poll_churn", "libev", end - start, NUM_ITERATIONS * NUM_WATCHERS * 2);

    ev_loop_destroy(loop);
}

// --- libevent ---
static void bench_libevent_churn(int fds[NUM_WATCHERS])
{
    struct event_base *base = event_base_new();
    struct event *ev[NUM_WATCHERS];

    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        ev[i] = event_new(base, fds[i], LIBEVENT_READ | LIBEVENT_WRITE, dummy_libevent_cb, NULL);
    }

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            event_add(ev[j], NULL);
        }
        event_base_loop(base, EVLOOP_NONBLOCK);

        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            event_del(ev[j]);
        }
        event_base_loop(base, EVLOOP_NONBLOCK);
    }
    uint64_t end = get_time_ns();

    print_benchmark("poll_churn", "libevent", end - start, NUM_ITERATIONS * NUM_WATCHERS * 2);

    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        event_free(ev[i]);
    }
    event_base_free(base);
}

// --- libuv ---
static void bench_libuv_churn(int fds[NUM_WATCHERS])
{
    uv_loop_t *loop = uv_loop_new();
    uv_poll_t io[NUM_WATCHERS];

    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        uv_poll_init(loop, &io[i], fds[i]);
    }

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            uv_poll_start(&io[j], UV_READABLE | UV_WRITABLE, dummy_libuv_cb);
        }
        uv_run(loop, UV_RUN_NOWAIT);

        for (size_t j = 0; j < NUM_WATCHERS; ++j) {
            uv_poll_stop(&io[j]);
        }
        uv_run(loop, UV_RUN_NOWAIT);
    }
    uint64_t end = get_time_ns();

    print_benchmark("poll_churn", "libuv", end - start, NUM_ITERATIONS * NUM_WATCHERS * 2);

    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        if (!uv_is_closing((uv_handle_t *)&io[i])) {
            uv_close((uv_handle_t *)&io[i], NULL);
        }
    }
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    free(loop);
}

int main(void)
{
    print_versions();

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif

    int fds[NUM_WATCHERS];
    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        fds[i] = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

    bench_evio_churn(fds, false);
    bench_evio_churn(fds, true);

    bench_libev_churn(fds);
    bench_libevent_churn(fds);
    bench_libuv_churn(fds);

    for (size_t i = 0; i < NUM_WATCHERS; ++i) {
        close(fds[i]);
    }

    return EXIT_SUCCESS;
}
