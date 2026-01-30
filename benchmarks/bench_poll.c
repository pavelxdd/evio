#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

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

#define NUM_PINGS 800000
#define MSG_SIZE 64
#define BATCH 8

// --- evio ---
typedef struct {
    evio_poll reader_watcher;
    evio_poll writer_watcher;
    int read_fd;
    int write_fd;
    size_t reads;
    size_t read_accum;
    size_t writes;
    char buf[MSG_SIZE * BATCH];
} evio_poll_ctx;

static void evio_read_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_poll_ctx *ctx = (evio_poll_ctx *)base->data;
    for (size_t i = 0; i < BATCH && ctx->reads < NUM_PINGS; ++i) {
        ssize_t n = read(ctx->read_fd, ctx->buf, sizeof(ctx->buf));
        if (n <= 0) {
            break;
        }
        ctx->read_accum += (size_t)n;
        size_t msgs = ctx->read_accum / MSG_SIZE;
        size_t left = NUM_PINGS - ctx->reads;
        if (msgs > left) {
            msgs = left;
        }
        ctx->reads += msgs;
        ctx->read_accum -= msgs * MSG_SIZE;
    }
    if (ctx->writes < NUM_PINGS) {
        evio_poll_start(loop, &ctx->writer_watcher);
    } else if (ctx->reads == NUM_PINGS) {
        evio_break(loop, EVIO_BREAK_ALL);
    }
    evio_poll_stop(loop, (evio_poll *)base);
}

static void evio_write_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_poll_ctx *ctx = (evio_poll_ctx *)base->data;
    char buf[MSG_SIZE * BATCH];
    size_t todo = NUM_PINGS - ctx->writes;
    if (todo > BATCH) {
        todo = BATCH;
    }
    if (todo) {
        size_t bytes = todo * MSG_SIZE;
        memset(buf, 'p', bytes);
        if (write(ctx->write_fd, buf, bytes) > 0) {
            ctx->writes += todo;
        }
    }
    evio_poll_start(loop, &ctx->reader_watcher);
    evio_poll_stop(loop, (evio_poll *)base);
}

static void bench_evio_poll(bool use_uring)
{
    int fds[2] = { -1, -1 };
    pipe(fds);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);

    evio_loop *loop = evio_loop_new(use_uring ? EVIO_FLAG_URING : EVIO_FLAG_NONE);

    evio_poll_ctx ctx = {
        .read_fd = fds[0],
        .write_fd = fds[1],
    };

    evio_poll_init(&ctx.reader_watcher, evio_read_cb, ctx.read_fd, EVIO_READ);
    ctx.reader_watcher.data = &ctx;

    evio_poll_init(&ctx.writer_watcher, evio_write_cb, ctx.write_fd, EVIO_WRITE);
    ctx.writer_watcher.data = &ctx;

    uint64_t start = get_time_ns();
    evio_poll_start(loop, &ctx.writer_watcher);
    evio_run(loop, EVIO_RUN_DEFAULT);
    uint64_t end = get_time_ns();

    print_benchmark("poll_ping_pong", use_uring ? "evio-uring" : "evio", end - start, NUM_PINGS);
    evio_loop_free(loop);
    close(fds[0]);
    close(fds[1]);
}

// --- libev ---
typedef struct {
    ev_io reader_watcher;
    ev_io writer_watcher;
    int read_fd;
    int write_fd;
    size_t reads;
    size_t read_accum;
    size_t writes;
    char buf[MSG_SIZE * BATCH];
} libev_poll_ctx;

static void libev_read_cb(struct ev_loop *loop, ev_io *w, int revents)
{
    libev_poll_ctx *ctx = w->data;
    for (size_t i = 0; i < BATCH && ctx->reads < NUM_PINGS; ++i) {
        ssize_t n = read(w->fd, ctx->buf, sizeof(ctx->buf));
        if (n <= 0) {
            break;
        }
        ctx->read_accum += (size_t)n;
        size_t msgs = ctx->read_accum / MSG_SIZE;
        size_t left = NUM_PINGS - ctx->reads;
        if (msgs > left) {
            msgs = left;
        }
        ctx->reads += msgs;
        ctx->read_accum -= msgs * MSG_SIZE;
    }
    if (ctx->writes < NUM_PINGS) {
        ev_io_start(loop, &ctx->writer_watcher);
    } else if (ctx->reads == NUM_PINGS) {
        ev_break(loop, EVBREAK_ALL);
    }
    ev_io_stop(loop, w);
}

static void libev_write_cb(struct ev_loop *loop, ev_io *w, int revents)
{
    libev_poll_ctx *ctx = w->data;
    char buf[MSG_SIZE * BATCH];
    size_t todo = NUM_PINGS - ctx->writes;
    if (todo > BATCH) {
        todo = BATCH;
    }
    if (todo) {
        size_t bytes = todo * MSG_SIZE;
        memset(buf, 'p', bytes);
        if (write(w->fd, buf, bytes) > 0) {
            ctx->writes += todo;
        }
    }
    ev_io_start(loop, &ctx->reader_watcher);
    ev_io_stop(loop, w);
}

static void bench_libev_poll(void)
{
    int fds[2] = { -1, -1 };
    pipe(fds);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);

    struct ev_loop *loop = ev_loop_new(0);

    libev_poll_ctx ctx = {
        .read_fd = fds[0],
        .write_fd = fds[1],
    };

    ev_io_init(&ctx.reader_watcher, libev_read_cb, ctx.read_fd, LIBEV_READ);
    ctx.reader_watcher.data = &ctx;

    ev_io_init(&ctx.writer_watcher, libev_write_cb, ctx.write_fd, LIBEV_WRITE);
    ctx.writer_watcher.data = &ctx;

    uint64_t start = get_time_ns();
    ev_io_start(loop, &ctx.writer_watcher);
    ev_run(loop, 0);
    uint64_t end = get_time_ns();

    print_benchmark("poll_ping_pong", "libev", end - start, NUM_PINGS);
    ev_loop_destroy(loop);
    close(fds[0]);
    close(fds[1]);
}

// --- libevent ---
typedef struct {
    struct event_base *base;
    struct event *reader;
    struct event *writer;
    int read_fd;
    int write_fd;
    size_t reads;
    size_t read_accum;
    size_t writes;
    char buf[MSG_SIZE * BATCH];
} libevent_poll_ctx;

static void libevent_write_cb(evutil_socket_t fd, short what, void *arg);

static void libevent_read_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;

    libevent_poll_ctx *ctx = arg;
    for (size_t i = 0; i < BATCH && ctx->reads < NUM_PINGS; ++i) {
        ssize_t n = read(ctx->read_fd, ctx->buf, sizeof(ctx->buf));
        if (n <= 0) {
            break;
        }
        ctx->read_accum += (size_t)n;
        size_t msgs = ctx->read_accum / MSG_SIZE;
        size_t left = NUM_PINGS - ctx->reads;
        if (msgs > left) {
            msgs = left;
        }
        ctx->reads += msgs;
        ctx->read_accum -= msgs * MSG_SIZE;
    }

    event_del(ctx->reader);

    if (ctx->writes < NUM_PINGS) {
        event_add(ctx->writer, NULL);
    } else if (ctx->reads == NUM_PINGS) {
        event_base_loopbreak(ctx->base);
    }
}

static void libevent_write_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;

    libevent_poll_ctx *ctx = arg;
    char buf[MSG_SIZE * BATCH];
    size_t todo = NUM_PINGS - ctx->writes;
    if (todo > BATCH) {
        todo = BATCH;
    }
    if (todo) {
        size_t bytes = todo * MSG_SIZE;
        memset(buf, 'p', bytes);
        if (write(ctx->write_fd, buf, bytes) > 0) {
            ctx->writes += todo;
        }
    }

    event_del(ctx->writer);
    event_add(ctx->reader, NULL);
}

static void bench_libevent_poll(void)
{
    int fds[2] = { -1, -1 };
    pipe(fds);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);

    struct event_base *base = event_base_new();

    libevent_poll_ctx ctx = {
        .base = base,
        .read_fd = fds[0],
        .write_fd = fds[1],
    };

    ctx.reader = event_new(base, ctx.read_fd, LIBEVENT_READ, libevent_read_cb, &ctx);
    ctx.writer = event_new(base, ctx.write_fd, LIBEVENT_WRITE, libevent_write_cb, &ctx);

    uint64_t start = get_time_ns();
    event_add(ctx.writer, NULL);
    event_base_dispatch(base);
    uint64_t end = get_time_ns();

    print_benchmark("poll_ping_pong", "libevent", end - start, NUM_PINGS);

    event_free(ctx.reader);
    event_free(ctx.writer);
    event_base_free(base);
    close(fds[0]);
    close(fds[1]);
}

// --- libuv ---
typedef struct {
    uv_loop_t *loop;
    uv_poll_t reader_watcher;
    uv_poll_t writer_watcher;
    int read_fd;
    int write_fd;
    size_t reads;
    size_t read_accum;
    size_t writes;
    char buf[MSG_SIZE * BATCH];
} libuv_poll_ctx;

static void libuv_write_cb(uv_poll_t *w, int status, int events);

static void libuv_read_cb(uv_poll_t *w, int status, int events)
{
    libuv_poll_ctx *ctx = w->data;
    for (size_t i = 0; i < BATCH && ctx->reads < NUM_PINGS; ++i) {
        ssize_t n = read(ctx->read_fd, ctx->buf, sizeof(ctx->buf));
        if (n <= 0) {
            break;
        }
        ctx->read_accum += (size_t)n;
        size_t msgs = ctx->read_accum / MSG_SIZE;
        size_t left = NUM_PINGS - ctx->reads;
        if (msgs > left) {
            msgs = left;
        }
        ctx->reads += msgs;
        ctx->read_accum -= msgs * MSG_SIZE;
    }
    uv_poll_stop(w);

    if (ctx->writes < NUM_PINGS) {
        uv_poll_start(&ctx->writer_watcher, UV_WRITABLE, (uv_poll_cb)libuv_write_cb);
    } else if (ctx->reads == NUM_PINGS) {
        uv_stop(ctx->loop);
    }
}

static void libuv_write_cb(uv_poll_t *w, int status, int events)
{
    libuv_poll_ctx *ctx = w->data;
    char buf[MSG_SIZE * BATCH];
    size_t todo = NUM_PINGS - ctx->writes;
    if (todo > BATCH) {
        todo = BATCH;
    }
    if (todo) {
        size_t bytes = todo * MSG_SIZE;
        memset(buf, 'p', bytes);
        if (write(ctx->write_fd, buf, bytes) > 0) {
            ctx->writes += todo;
        }
    }
    uv_poll_stop(w);
    uv_poll_start(&ctx->reader_watcher, UV_READABLE, (uv_poll_cb)libuv_read_cb);
}

static void bench_libuv_poll(void)
{
    int fds[2] = { -1, -1 };
    pipe(fds);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);

    uv_loop_t *loop = uv_loop_new();

    libuv_poll_ctx ctx = {
        .loop = loop,
        .read_fd = fds[0],
        .write_fd = fds[1],
    };

    uv_poll_init(loop, &ctx.reader_watcher, ctx.read_fd);
    ctx.reader_watcher.data = &ctx;

    uv_poll_init(loop, &ctx.writer_watcher, ctx.write_fd);
    ctx.writer_watcher.data = &ctx;

    uint64_t start = get_time_ns();
    uv_poll_start(&ctx.writer_watcher, UV_WRITABLE, libuv_write_cb);
    uv_run(loop, UV_RUN_DEFAULT);
    uint64_t end = get_time_ns();

    print_benchmark("poll_ping_pong", "libuv", end - start, NUM_PINGS);

    uv_close((uv_handle_t *)&ctx.reader_watcher, NULL);
    uv_close((uv_handle_t *)&ctx.writer_watcher, NULL);
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    free(loop);
    close(fds[0]);
    close(fds[1]);
}

int main(void)
{
    print_versions();
    bench_evio_poll(false);
    bench_evio_poll(true);
    bench_libev_poll();
    bench_libevent_poll();
    bench_libuv_poll();
    return EXIT_SUCCESS;
}
