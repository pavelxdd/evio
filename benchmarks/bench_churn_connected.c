#include <stdlib.h>
#include <unistd.h>

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

#define NUM_PIPES 256
#define NUM_ITERATIONS 10000

static void dummy_evio_cb(evio_loop *loop, evio_base *base, evio_mask emask) {}
static void dummy_libev_cb(struct ev_loop *loop, ev_io *w, int revents) {}
static void dummy_libuv_cb(uv_poll_t *w, int status, int events) {}
static void dummy_libevent_cb(evutil_socket_t fd, short what, void *arg) {}

static void bench_evio(int fds[NUM_PIPES], bool use_uring)
{
    evio_loop *loop = evio_loop_new(use_uring ? EVIO_FLAG_URING : EVIO_FLAG_NONE);
    evio_poll io[NUM_PIPES];

    for (size_t i = 0; i < NUM_PIPES; ++i) {
        evio_poll_init(&io[i], dummy_evio_cb, fds[i], EVIO_READ);
    }

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        for (size_t j = 0; j < NUM_PIPES; ++j) {
            evio_poll_start(loop, &io[j]);
        }
        evio_run(loop, EVIO_RUN_NOWAIT);

        for (size_t j = 0; j < NUM_PIPES; ++j) {
            evio_poll_stop(loop, &io[j]);
        }
        evio_run(loop, EVIO_RUN_NOWAIT);
    }
    uint64_t end = get_time_ns();

    const char *name = use_uring ? "evio-uring" : "evio";
    print_benchmark("churn_connected", name, end - start, NUM_ITERATIONS * NUM_PIPES * 2);

    evio_loop_free(loop);
}

static void bench_libev(int fds[NUM_PIPES])
{
    struct ev_loop *loop = ev_loop_new(0);
    ev_io io[NUM_PIPES];

    for (size_t i = 0; i < NUM_PIPES; ++i) {
        ev_io_init(&io[i], dummy_libev_cb, fds[i], LIBEV_READ);
    }

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        for (size_t j = 0; j < NUM_PIPES; ++j) {
            ev_io_start(loop, &io[j]);
        }
        ev_run(loop, EVRUN_NOWAIT);

        for (size_t j = 0; j < NUM_PIPES; ++j) {
            ev_io_stop(loop, &io[j]);
        }
        ev_run(loop, EVRUN_NOWAIT);
    }
    uint64_t end = get_time_ns();

    print_benchmark("churn_connected", "libev", end - start, NUM_ITERATIONS * NUM_PIPES * 2);

    ev_loop_destroy(loop);
}

static void bench_libevent(int fds[NUM_PIPES])
{
    struct event_base *base = event_base_new();
    struct event *ev[NUM_PIPES];

    for (size_t i = 0; i < NUM_PIPES; ++i) {
        ev[i] = event_new(base, fds[i], LIBEVENT_READ, dummy_libevent_cb, NULL);
    }

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        for (size_t j = 0; j < NUM_PIPES; ++j) {
            event_add(ev[j], NULL);
        }
        event_base_loop(base, EVLOOP_NONBLOCK);

        for (size_t j = 0; j < NUM_PIPES; ++j) {
            event_del(ev[j]);
        }
        event_base_loop(base, EVLOOP_NONBLOCK);
    }
    uint64_t end = get_time_ns();

    print_benchmark("churn_connected", "libevent", end - start, NUM_ITERATIONS * NUM_PIPES * 2);

    for (size_t i = 0; i < NUM_PIPES; ++i) {
        event_free(ev[i]);
    }
    event_base_free(base);
}

static void bench_libuv(int fds[NUM_PIPES])
{
    uv_loop_t *loop = uv_loop_new();
    uv_poll_t io[NUM_PIPES];

    for (size_t i = 0; i < NUM_PIPES; ++i) {
        uv_poll_init(loop, &io[i], fds[i]);
    }

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        for (size_t j = 0; j < NUM_PIPES; ++j) {
            uv_poll_start(&io[j], UV_READABLE, dummy_libuv_cb);
        }
        uv_run(loop, UV_RUN_NOWAIT);

        for (size_t j = 0; j < NUM_PIPES; ++j) {
            uv_poll_stop(&io[j]);
        }
        uv_run(loop, UV_RUN_NOWAIT);
    }
    uint64_t end = get_time_ns();

    print_benchmark("churn_connected", "libuv", end - start, NUM_ITERATIONS * NUM_PIPES * 2);

    for (size_t i = 0; i < NUM_PIPES; ++i) {
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

    int pipes[NUM_PIPES][2];
    int fds[NUM_PIPES];
    for (size_t i = 0; i < NUM_PIPES; ++i) {
        if (pipe(pipes[i]) < 0) {
            abort();
        }
        fds[i] = pipes[i][0];
    }

    bench_evio(fds, false);
    bench_evio(fds, true);

    bench_libev(fds);
    bench_libevent(fds);
    bench_libuv(fds);

    for (size_t i = 0; i < NUM_PIPES; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    return EXIT_SUCCESS;
}
