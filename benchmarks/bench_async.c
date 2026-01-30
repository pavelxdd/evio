#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>

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

#define NUM_PINGS 100000

// --- evio ---
typedef struct {
    evio_loop *loop;
    evio_async async;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} evio_async_ctx;

static void evio_async_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_async_ctx *ctx = base->data;
    pthread_mutex_lock(&ctx->mutex);
    ctx->count++;
    if (ctx->count == NUM_PINGS) {
        evio_break(loop, EVIO_BREAK_ALL);
    }
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);
}

static void *evio_sender_thread(void *arg)
{
    evio_async_ctx *ctx = arg;
    for (size_t i = 0; i < NUM_PINGS; ++i) {
        evio_async_send(ctx->loop, &ctx->async);

        pthread_mutex_lock(&ctx->mutex);
        while (ctx->count <= i) {
            pthread_cond_wait(&ctx->cond, &ctx->mutex);
        }
        pthread_mutex_unlock(&ctx->mutex);
    }
    return NULL;
}

static void bench_evio_async(bool use_uring)
{
    evio_async_ctx ctx = {
        .loop = evio_loop_new(use_uring ? EVIO_FLAG_URING : EVIO_FLAG_NONE),
    };
    pthread_mutex_init(&ctx.mutex, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    evio_async_init(&ctx.async, evio_async_cb);
    ctx.async.data = &ctx;
    evio_async_start(ctx.loop, &ctx.async);

    pthread_t sender;
    uint64_t start = get_time_ns();
    pthread_create(&sender, NULL, evio_sender_thread, &ctx);
    evio_run(ctx.loop, EVIO_RUN_DEFAULT);
    uint64_t end = get_time_ns();
    pthread_join(sender, NULL);

    print_benchmark("async_ping_pong", use_uring ? "evio-uring" : "evio", end - start, NUM_PINGS);
    evio_loop_free(ctx.loop);
    pthread_mutex_destroy(&ctx.mutex);
    pthread_cond_destroy(&ctx.cond);
}

// --- libev ---
typedef struct {
    struct ev_loop *loop;
    ev_async async;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} libev_async_ctx;

static void libev_async_cb(struct ev_loop *loop, ev_async *w, int revents)
{
    libev_async_ctx *ctx = w->data;
    pthread_mutex_lock(&ctx->mutex);
    ctx->count++;
    if (ctx->count == NUM_PINGS) {
        ev_break(loop, EVBREAK_ALL);
    }
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);
}

static void *libev_sender_thread(void *arg)
{
    libev_async_ctx *ctx = arg;
    for (size_t i = 0; i < NUM_PINGS; ++i) {
        ev_async_send(ctx->loop, &ctx->async);

        pthread_mutex_lock(&ctx->mutex);
        while (ctx->count <= i) {
            pthread_cond_wait(&ctx->cond, &ctx->mutex);
        }
        pthread_mutex_unlock(&ctx->mutex);
    }
    return NULL;
}

static void bench_libev_async(void)
{
    libev_async_ctx ctx = {
        .loop = ev_loop_new(0),
    };
    pthread_mutex_init(&ctx.mutex, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    ev_async_init(&ctx.async, libev_async_cb);
    ctx.async.data = &ctx;
    ev_async_start(ctx.loop, &ctx.async);

    pthread_t sender;
    uint64_t start = get_time_ns();
    pthread_create(&sender, NULL, libev_sender_thread, &ctx);
    ev_run(ctx.loop, 0);
    uint64_t end = get_time_ns();
    pthread_join(sender, NULL);

    print_benchmark("async_ping_pong", "libev", end - start, NUM_PINGS);
    ev_loop_destroy(ctx.loop);
    pthread_mutex_destroy(&ctx.mutex);
    pthread_cond_destroy(&ctx.cond);
}

// --- libevent ---
typedef struct {
    struct event_base *base;
    struct event *ev;
    int fd;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} libevent_async_ctx;

static void libevent_async_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;

    libevent_async_ctx *ctx = arg;
    eventfd_t total = 0;
    for (;;) {
        eventfd_t val = 0;
        ssize_t n = read(ctx->fd, &val, sizeof(val));
        if (n > 0) {
            total += val;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    pthread_mutex_lock(&ctx->mutex);
    ctx->count += (size_t)total;
    if (ctx->count >= NUM_PINGS) {
        event_base_loopbreak(ctx->base);
    }
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);
}

static void *libevent_sender_thread(void *arg)
{
    libevent_async_ctx *ctx = arg;
    for (size_t i = 0; i < NUM_PINGS; ++i) {
        eventfd_t one = 1;
        for (;;) {
            ssize_t n = write(ctx->fd, &one, sizeof(one));
            if (n >= 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                eventfd_t tmp;
                (void)read(ctx->fd, &tmp, sizeof(tmp));
                continue;
            }
            break;
        }

        pthread_mutex_lock(&ctx->mutex);
        while (ctx->count <= i) {
            pthread_cond_wait(&ctx->cond, &ctx->mutex);
        }
        pthread_mutex_unlock(&ctx->mutex);
    }
    return NULL;
}

static void bench_libevent_async(void)
{
    libevent_async_ctx ctx = {
        .base = event_base_new(),
    };
    pthread_mutex_init(&ctx.mutex, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    ctx.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ctx.ev = event_new(ctx.base, ctx.fd, LIBEVENT_READ | EV_PERSIST, libevent_async_cb, &ctx);
    event_add(ctx.ev, NULL);

    pthread_t sender;
    uint64_t start = get_time_ns();
    pthread_create(&sender, NULL, libevent_sender_thread, &ctx);
    event_base_dispatch(ctx.base);
    uint64_t end = get_time_ns();
    pthread_join(sender, NULL);

    print_benchmark("async_ping_pong", "libevent", end - start, NUM_PINGS);

    event_del(ctx.ev);
    event_free(ctx.ev);
    close(ctx.fd);
    event_base_free(ctx.base);
    pthread_mutex_destroy(&ctx.mutex);
    pthread_cond_destroy(&ctx.cond);
}

// --- libuv ---
typedef struct {
    uv_loop_t *loop;
    uv_async_t async;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} libuv_async_ctx;

static void libuv_async_cb(uv_async_t *handle)
{
    libuv_async_ctx *ctx = handle->data;
    pthread_mutex_lock(&ctx->mutex);
    ctx->count++;
    if (ctx->count == NUM_PINGS) {
        uv_stop(ctx->loop);
    }
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);
}

static void *libuv_sender_thread(void *arg)
{
    libuv_async_ctx *ctx = arg;
    for (size_t i = 0; i < NUM_PINGS; ++i) {
        uv_async_send(&ctx->async);

        pthread_mutex_lock(&ctx->mutex);
        while (ctx->count <= i) {
            pthread_cond_wait(&ctx->cond, &ctx->mutex);
        }
        pthread_mutex_unlock(&ctx->mutex);
    }
    return NULL;
}

static void bench_libuv_async(void)
{
    libuv_async_ctx ctx = {
        .loop = uv_loop_new(),
    };
    pthread_mutex_init(&ctx.mutex, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    uv_async_init(ctx.loop, &ctx.async, libuv_async_cb);
    ctx.async.data = &ctx;

    pthread_t sender;
    uint64_t start = get_time_ns();
    pthread_create(&sender, NULL, libuv_sender_thread, &ctx);
    uv_run(ctx.loop, UV_RUN_DEFAULT);
    uint64_t end = get_time_ns();
    pthread_join(sender, NULL);

    print_benchmark("async_ping_pong", "libuv", end - start, NUM_PINGS);

    uv_close((uv_handle_t *)&ctx.async, NULL);
    uv_run(ctx.loop, UV_RUN_NOWAIT);

    uv_loop_close(ctx.loop);
    free(ctx.loop);
    pthread_mutex_destroy(&ctx.mutex);
    pthread_cond_destroy(&ctx.cond);
}

int main(void)
{
    print_versions();
    bench_evio_async(false);
    bench_evio_async(true);
    bench_libev_async();
    bench_libevent_async();
    bench_libuv_async();
    return EXIT_SUCCESS;
}
