#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>

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

#define NUM_SIGNALS 2000000

// --- evio ---
static void evio_signal_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    size_t *count = base->data;
    ++(*count);
}

static void bench_evio_signal(bool use_uring)
{
    evio_loop *loop = evio_loop_new(use_uring ? EVIO_FLAG_URING : EVIO_FLAG_NONE);

    evio_signal sig;
    evio_signal_init(&sig, evio_signal_cb, SIGUSR1);
    evio_signal_start(loop, &sig);

    size_t count = 0;
    sig.data = &count;

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_SIGNALS; ++i) {
        kill(getpid(), SIGUSR1);
        evio_run(loop, EVIO_RUN_NOWAIT);
    }
    while (count < NUM_SIGNALS) {
        evio_run(loop, EVIO_RUN_ONCE);
    }
    uint64_t end = get_time_ns();

    print_benchmark("signal_delivery", use_uring ? "evio-uring" : "evio", end - start, count);
    evio_signal_stop(loop, &sig);
    evio_loop_free(loop);
}

// --- libev ---
static void libev_signal_cb(struct ev_loop *loop, ev_signal *w, int revents)
{
    size_t *count = w->data;
    ++(*count);
}

static void bench_libev_signal(void)
{
    struct ev_loop *loop = ev_loop_new(0);

    ev_signal sig;
    ev_signal_init(&sig, libev_signal_cb, SIGUSR1);
    ev_signal_start(loop, &sig);

    size_t count = 0;
    sig.data = &count;

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_SIGNALS; ++i) {
        kill(getpid(), SIGUSR1);
        ev_run(loop, EVRUN_NOWAIT);
    }
    while (count < NUM_SIGNALS) {
        ev_run(loop, EVRUN_ONCE);
    }
    uint64_t end = get_time_ns();

    print_benchmark("signal_delivery", "libev", end - start, count);
    ev_signal_stop(loop, &sig);
    ev_loop_destroy(loop);
}

// --- libevent ---
static void libevent_signal_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;

    size_t *count = arg;
    ++(*count);
}

static void bench_libevent_signal(void)
{
    struct event_base *base = event_base_new();
    if (!base) {
        abort();
    }
    size_t count = 0;

    struct event *sig = evsignal_new(base, SIGUSR1, libevent_signal_cb, &count);
    if (!sig) {
        abort();
    }
    event_add(sig, NULL);

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_SIGNALS; ++i) {
        kill(getpid(), SIGUSR1);
        event_base_loop(base, EVLOOP_NONBLOCK);
    }
    while (count < NUM_SIGNALS) {
        event_base_loop(base, EVLOOP_ONCE);
    }
    uint64_t end = get_time_ns();

    print_benchmark("signal_delivery", "libevent", end - start, count);
    event_del(sig);
    event_free(sig);
    event_base_free(base);
}

// --- libuv ---
static void libuv_signal_cb(uv_signal_t *handle, int signum)
{
    size_t *count = handle->data;
    ++(*count);
}

static void bench_libuv_signal(void)
{
    uv_loop_t *loop = uv_loop_new();

    uv_signal_t sig;
    uv_signal_init(loop, &sig);
    uv_signal_start(&sig, libuv_signal_cb, SIGUSR1);

    size_t count = 0;
    sig.data = &count;

    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_SIGNALS; ++i) {
        kill(getpid(), SIGUSR1);
        uv_run(loop, UV_RUN_NOWAIT);
    }
    while (count < NUM_SIGNALS) {
        uv_run(loop, UV_RUN_ONCE);
    }
    uint64_t end = get_time_ns();

    print_benchmark("signal_delivery", "libuv", end - start, count);

    uv_signal_stop(&sig);
    // uv_close is async, need to run loop for it to complete.
    uv_close((uv_handle_t *)&sig, NULL);
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    free(loop);
}

int main(void)
{
    print_versions();
    bench_evio_signal(false);
    bench_evio_signal(true);
    bench_libev_signal();
    bench_libevent_signal();
    bench_libuv_signal();
    return EXIT_SUCCESS;
}
