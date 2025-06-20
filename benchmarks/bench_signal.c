#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>

#include <ev.h>
#include <uv.h>

#include "evio.h"
#include "bench.h"

#define NUM_SIGNALS 1000000

// --- evio ---
static void evio_signal_cb(evio_loop *loop, evio_base *w, evio_mask emask)
{
    size_t *count = w->data;
    ++(*count);
}

static void bench_evio_signal(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_URING);

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

    print_benchmark("signal_delivery", "evio", end - start, count);
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
    bench_evio_signal();
    bench_libev_signal();
    bench_libuv_signal();
    return EXIT_SUCCESS;
}
