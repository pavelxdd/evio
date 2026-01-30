#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>

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

#define DEF_WATCHERS 256u
#define DEF_ITERATIONS 2000u

#define MAX_WATCHERS 2048u
#define MAX_ITERATIONS 20000u

static void dummy_evio_cb(evio_loop *loop, evio_base *base, evio_mask emask) {}
static void dummy_libev_cb(struct ev_loop *loop, ev_io *w, int revents) {}
static void dummy_libuv_cb(uv_poll_t *w, int status, int events) {}
static void dummy_libevent_cb(evutil_socket_t fd, short what, void *arg) {}

static unsigned int env_u32(const char *name, unsigned int def, unsigned int max)
{
    const char *s = getenv(name);
    if (!s || !*s) {
        return def;
    }

    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end) {
        return def;
    }

    if (!v) {
        return def;
    }
    if (v > max) {
        return max;
    }
    return (unsigned int)v;
}

typedef struct {
    pthread_barrier_t *ready;
    pthread_barrier_t *start;
    unsigned int watchers;
    unsigned int iterations;
    bool use_uring;
} evio_thr_arg;

static void *evio_churn_thread(void *ptr)
{
    evio_thr_arg *arg = ptr;
    unsigned int watchers = arg->watchers;
    unsigned int iterations = arg->iterations;

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif

    int *fds = malloc(sizeof(*fds) * watchers);
    if (!fds) {
        abort();
    }
    for (unsigned int i = 0; i < watchers; ++i) {
        fds[i] = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fds[i] < 0) {
            abort();
        }
    }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

    evio_loop *loop = evio_loop_new(arg->use_uring ? EVIO_FLAG_URING : EVIO_FLAG_NONE);
    if (!loop) {
        abort();
    }
    evio_poll *io = malloc(sizeof(*io) * watchers);
    if (!io) {
        abort();
    }

    for (unsigned int i = 0; i < watchers; ++i) {
        evio_poll_init(&io[i], dummy_evio_cb, fds[i], EVIO_READ | EVIO_WRITE);
    }

    pthread_barrier_wait(arg->ready);
    pthread_barrier_wait(arg->start);

    for (unsigned int i = 0; i < iterations; ++i) {
        for (unsigned int j = 0; j < watchers; ++j) {
            evio_poll_start(loop, &io[j]);
        }
        evio_run(loop, EVIO_RUN_NOWAIT);

        for (unsigned int j = 0; j < watchers; ++j) {
            evio_poll_stop(loop, &io[j]);
        }
        evio_run(loop, EVIO_RUN_NOWAIT);
    }

    evio_loop_free(loop);

    for (unsigned int i = 0; i < watchers; ++i) {
        close(fds[i]);
    }

    free(io);
    free(fds);
    return NULL;
}

static void bench_evio_churn_mt(unsigned int threads, unsigned int watchers, unsigned int iterations,
                                bool use_uring)
{
    pthread_t *thr = malloc(sizeof(*thr) * threads);
    if (!thr) {
        abort();
    }
    pthread_barrier_t ready;
    pthread_barrier_t start;
    if (pthread_barrier_init(&ready, NULL, threads + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&start, NULL, threads + 1) != 0) {
        abort();
    }

    evio_thr_arg arg = {
        .ready = &ready,
        .start = &start,
        .watchers = watchers,
        .iterations = iterations,
        .use_uring = use_uring,
    };

    for (unsigned int i = 0; i < threads; ++i) {
        if (pthread_create(&thr[i], NULL, evio_churn_thread, &arg) != 0) {
            abort();
        }
    }

    pthread_barrier_wait(&ready);
    uint64_t t0 = get_time_ns();
    pthread_barrier_wait(&start);

    for (unsigned int i = 0; i < threads; ++i) {
        if (pthread_join(thr[i], NULL) != 0) {
            abort();
        }
    }
    uint64_t t1 = get_time_ns();

    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&ready);
    free(thr);

    char name[32];
    snprintf(name, sizeof(name), "evio%s-t%u", use_uring ? "-uring" : "", threads);
    print_benchmark("poll_churn_mt", name, t1 - t0,
                    (uint64_t)threads * iterations * watchers * 2);
}

typedef struct {
    pthread_barrier_t *ready;
    pthread_barrier_t *start;
    unsigned int watchers;
    unsigned int iterations;
} libev_thr_arg;

static void *libev_churn_thread(void *ptr)
{
    libev_thr_arg *arg = ptr;
    unsigned int watchers = arg->watchers;
    unsigned int iterations = arg->iterations;

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif

    int *fds = malloc(sizeof(*fds) * watchers);
    if (!fds) {
        abort();
    }
    for (unsigned int i = 0; i < watchers; ++i) {
        fds[i] = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fds[i] < 0) {
            abort();
        }
    }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

    struct ev_loop *loop = ev_loop_new(0);
    if (!loop) {
        abort();
    }
    ev_io *io = malloc(sizeof(*io) * watchers);
    if (!io) {
        abort();
    }

    for (unsigned int i = 0; i < watchers; ++i) {
        ev_io_init(&io[i], dummy_libev_cb, fds[i], LIBEV_READ | LIBEV_WRITE);
    }

    pthread_barrier_wait(arg->ready);
    pthread_barrier_wait(arg->start);

    for (unsigned int i = 0; i < iterations; ++i) {
        for (unsigned int j = 0; j < watchers; ++j) {
            ev_io_start(loop, &io[j]);
        }
        ev_run(loop, EVRUN_NOWAIT);

        for (unsigned int j = 0; j < watchers; ++j) {
            ev_io_stop(loop, &io[j]);
        }
        ev_run(loop, EVRUN_NOWAIT);
    }

    ev_loop_destroy(loop);

    for (unsigned int i = 0; i < watchers; ++i) {
        close(fds[i]);
    }

    free(io);
    free(fds);
    return NULL;
}

typedef struct {
    pthread_barrier_t *ready;
    pthread_barrier_t *start;
    unsigned int watchers;
    unsigned int iterations;
} libevent_thr_arg;

static void *libevent_churn_thread(void *ptr)
{
    libevent_thr_arg *arg = ptr;
    unsigned int watchers = arg->watchers;
    unsigned int iterations = arg->iterations;

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif

    int *fds = malloc(sizeof(*fds) * watchers);
    if (!fds) {
        abort();
    }
    for (unsigned int i = 0; i < watchers; ++i) {
        fds[i] = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fds[i] < 0) {
            abort();
        }
    }

    struct event_base *base = event_base_new();
    if (!base) {
        abort();
    }

    struct event **ev = malloc(sizeof(*ev) * watchers);
    if (!ev) {
        abort();
    }
    for (unsigned int i = 0; i < watchers; ++i) {
        ev[i] = event_new(base, fds[i], LIBEVENT_READ | LIBEVENT_WRITE, dummy_libevent_cb, NULL);
        if (!ev[i]) {
            abort();
        }
    }

    pthread_barrier_wait(arg->ready);
    pthread_barrier_wait(arg->start);

    for (unsigned int i = 0; i < iterations; ++i) {
        for (unsigned int j = 0; j < watchers; ++j) {
            event_add(ev[j], NULL);
        }
        event_base_loop(base, EVLOOP_NONBLOCK);

        for (unsigned int j = 0; j < watchers; ++j) {
            event_del(ev[j]);
        }
        event_base_loop(base, EVLOOP_NONBLOCK);
    }

    for (unsigned int i = 0; i < watchers; ++i) {
        event_free(ev[i]);
    }
    free(ev);

    event_base_free(base);

    for (unsigned int i = 0; i < watchers; ++i) {
        close(fds[i]);
    }
    free(fds);

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    return NULL;
}

static void bench_libevent_churn_mt(unsigned int threads, unsigned int watchers, unsigned int iterations)
{
    pthread_t *thr = malloc(sizeof(*thr) * threads);
    if (!thr) {
        abort();
    }
    pthread_barrier_t ready;
    pthread_barrier_t start;
    if (pthread_barrier_init(&ready, NULL, threads + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&start, NULL, threads + 1) != 0) {
        abort();
    }

    libevent_thr_arg arg = {
        .ready = &ready,
        .start = &start,
        .watchers = watchers,
        .iterations = iterations,
    };

    for (unsigned int i = 0; i < threads; ++i) {
        if (pthread_create(&thr[i], NULL, libevent_churn_thread, &arg) != 0) {
            abort();
        }
    }

    pthread_barrier_wait(&ready);

    uint64_t start_ns = get_time_ns();
    pthread_barrier_wait(&start);

    for (unsigned int i = 0; i < threads; ++i) {
        pthread_join(thr[i], NULL);
    }
    uint64_t end_ns = get_time_ns();

    pthread_barrier_destroy(&ready);
    pthread_barrier_destroy(&start);
    free(thr);

    char name[32];
    snprintf(name, sizeof(name), "libevent-t%u", threads);
    print_benchmark("poll_churn_mt", name, end_ns - start_ns, (uint64_t)threads * watchers * iterations * 2);
}
static void bench_libev_churn_mt(unsigned int threads, unsigned int watchers, unsigned int iterations)
{
    pthread_t *thr = malloc(sizeof(*thr) * threads);
    if (!thr) {
        abort();
    }
    pthread_barrier_t ready;
    pthread_barrier_t start;
    if (pthread_barrier_init(&ready, NULL, threads + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&start, NULL, threads + 1) != 0) {
        abort();
    }

    libev_thr_arg arg = {
        .ready = &ready,
        .start = &start,
        .watchers = watchers,
        .iterations = iterations,
    };

    for (unsigned int i = 0; i < threads; ++i) {
        if (pthread_create(&thr[i], NULL, libev_churn_thread, &arg) != 0) {
            abort();
        }
    }

    pthread_barrier_wait(&ready);
    uint64_t t0 = get_time_ns();
    pthread_barrier_wait(&start);

    for (unsigned int i = 0; i < threads; ++i) {
        if (pthread_join(thr[i], NULL) != 0) {
            abort();
        }
    }
    uint64_t t1 = get_time_ns();

    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&ready);
    free(thr);

    char name[32];
    snprintf(name, sizeof(name), "libev-t%u", threads);
    print_benchmark("poll_churn_mt", name, t1 - t0,
                    (uint64_t)threads * iterations * watchers * 2);
}

typedef struct {
    pthread_barrier_t *ready;
    pthread_barrier_t *start;
    unsigned int watchers;
    unsigned int iterations;
} libuv_thr_arg;

static void *libuv_churn_thread(void *ptr)
{
    libuv_thr_arg *arg = ptr;
    unsigned int watchers = arg->watchers;
    unsigned int iterations = arg->iterations;

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif

    int *fds = malloc(sizeof(*fds) * watchers);
    if (!fds) {
        abort();
    }
    for (unsigned int i = 0; i < watchers; ++i) {
        fds[i] = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fds[i] < 0) {
            abort();
        }
    }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

    uv_loop_t *loop = uv_loop_new();
    if (!loop) {
        abort();
    }
    uv_poll_t *io = malloc(sizeof(*io) * watchers);
    if (!io) {
        abort();
    }

    for (unsigned int i = 0; i < watchers; ++i) {
        uv_poll_init(loop, &io[i], fds[i]);
    }

    pthread_barrier_wait(arg->ready);
    pthread_barrier_wait(arg->start);

    for (unsigned int i = 0; i < iterations; ++i) {
        for (unsigned int j = 0; j < watchers; ++j) {
            uv_poll_start(&io[j], UV_READABLE | UV_WRITABLE, dummy_libuv_cb);
        }
        uv_run(loop, UV_RUN_NOWAIT);

        for (unsigned int j = 0; j < watchers; ++j) {
            uv_poll_stop(&io[j]);
        }
        uv_run(loop, UV_RUN_NOWAIT);
    }

    for (unsigned int i = 0; i < watchers; ++i) {
        if (!uv_is_closing((uv_handle_t *)&io[i])) {
            uv_close((uv_handle_t *)&io[i], NULL);
        }
    }
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    free(loop);

    for (unsigned int i = 0; i < watchers; ++i) {
        close(fds[i]);
    }

    free(io);
    free(fds);
    return NULL;
}

static void bench_libuv_churn_mt(unsigned int threads, unsigned int watchers, unsigned int iterations)
{
    pthread_t *thr = malloc(sizeof(*thr) * threads);
    if (!thr) {
        abort();
    }
    pthread_barrier_t ready;
    pthread_barrier_t start;
    if (pthread_barrier_init(&ready, NULL, threads + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&start, NULL, threads + 1) != 0) {
        abort();
    }

    libuv_thr_arg arg = {
        .ready = &ready,
        .start = &start,
        .watchers = watchers,
        .iterations = iterations,
    };

    for (unsigned int i = 0; i < threads; ++i) {
        if (pthread_create(&thr[i], NULL, libuv_churn_thread, &arg) != 0) {
            abort();
        }
    }

    pthread_barrier_wait(&ready);
    uint64_t t0 = get_time_ns();
    pthread_barrier_wait(&start);

    for (unsigned int i = 0; i < threads; ++i) {
        if (pthread_join(thr[i], NULL) != 0) {
            abort();
        }
    }
    uint64_t t1 = get_time_ns();

    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&ready);
    free(thr);

    char name[32];
    snprintf(name, sizeof(name), "libuv-t%u", threads);
    print_benchmark("poll_churn_mt", name, t1 - t0,
                    (uint64_t)threads * iterations * watchers * 2);
}

int main(void)
{
    print_versions();

    unsigned int watchers = env_u32("EVIO_BENCH_MT_WATCHERS", DEF_WATCHERS, MAX_WATCHERS);
    unsigned int iterations = env_u32("EVIO_BENCH_MT_ITERS", DEF_ITERATIONS, MAX_ITERATIONS);

    unsigned int ncpu = (unsigned int)sysconf(_SC_NPROCESSORS_ONLN);
    if (!ncpu) {
        ncpu = 1;
    }

    const unsigned int max_threads = ncpu < 8 ? ncpu : 8;

    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur != RLIM_INFINITY) {
        unsigned long long cur = lim.rlim_cur;
        unsigned long long margin = 128;
        unsigned long long max_watchers = (cur > margin) ? ((cur - margin) / max_threads) : 1;
        if (!max_watchers) {
            max_watchers = 1;
        }
        if (watchers > max_watchers) {
            watchers = (unsigned int)max_watchers;
        }
    }

    const unsigned int thread_counts[] = { 1, 2, 4, 8 };

    for (size_t i = 0; i < sizeof(thread_counts) / sizeof(thread_counts[0]); ++i) {
        unsigned int t = thread_counts[i];
        if (t > max_threads) {
            continue;
        }

        bench_evio_churn_mt(t, watchers, iterations, false);
        bench_evio_churn_mt(t, watchers, iterations, true);
        bench_libev_churn_mt(t, watchers, iterations);
        bench_libevent_churn_mt(t, watchers, iterations);
        bench_libuv_churn_mt(t, watchers, iterations);
        printf("\n");
    }

    return EXIT_SUCCESS;
}
