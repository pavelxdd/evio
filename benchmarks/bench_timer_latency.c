#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "evio.h"

#define NUM_DEADLINES 1000
#define NUM_READY_FDS 256
#define TIMER_PERIOD EVIO_TIME_FROM_MSEC(2)

typedef struct {
    evio_time deadline;
    evio_time delays[NUM_DEADLINES];
    size_t samples;
    size_t missed;
    uint64_t io_cycles;
    bool io_failed;
} timer_latency_ctx;

static void ready_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_poll *w = (evio_poll *)base;
    timer_latency_ctx *ctx = base->data;
    eventfd_t value;
    if (eventfd_read(w->fd, &value) < 0 || eventfd_write(w->fd, value) < 0) {
        perror("eventfd I/O");
        ctx->io_failed = true;
        evio_break(loop, EVIO_BREAK_ALL);
        return;
    }
    ++ctx->io_cycles;
}

static void timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    timer_latency_ctx *ctx = base->data;
    evio_update_time(loop);
    evio_time now = evio_get_time(loop);
    evio_time delay = now - ctx->deadline;
    ctx->delays[ctx->samples++] = delay;

    // Sample the oldest due deadline and count the other elapsed periods as missed.
    evio_time skipped = delay / TIMER_PERIOD;
    size_t remaining = NUM_DEADLINES - ctx->samples - ctx->missed;
    if (skipped >= remaining) {
        ctx->missed += remaining;
        evio_break(loop, EVIO_BREAK_ALL);
        return;
    }
    ctx->missed += skipped;
    ctx->deadline += (skipped + 1) * TIMER_PERIOD;
    evio_timer_start(loop, (evio_timer *)base, ctx->deadline - now);
}

static int compare_delays(const void *a, const void *b)
{
    evio_time lhs = *(const evio_time *)a;
    evio_time rhs = *(const evio_time *)b;
    return (lhs > rhs) - (lhs < rhs);
}

static bool bench_timer_latency(size_t fd_count)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        perror("evio_loop_new");
        return false;
    }
    evio_set_clockid(loop, CLOCK_MONOTONIC);

    timer_latency_ctx ctx = { 0 };
    evio_poll watchers[NUM_READY_FDS];
    size_t started = 0;
    bool ok = false;

    for (; started < fd_count; ++started) {
        int fd = eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) {
            perror("eventfd");
            goto out;
        }
        evio_poll *w = &watchers[started];
        evio_poll_init(w, ready_cb, fd, EVIO_READ);
        w->data = &ctx;
        evio_poll_start(loop, w);
    }

    if (fd_count) {
        evio_run(loop, EVIO_RUN_NOWAIT);
        if (ctx.io_failed) {
            goto out;
        }
        ctx.io_cycles = 0;
    }

    evio_timer timer;
    evio_timer_init(&timer, timer_cb, 0);
    timer.data = &ctx;
    evio_update_time(loop);
    ctx.deadline = evio_get_time(loop) + TIMER_PERIOD;
    evio_timer_start(loop, &timer, TIMER_PERIOD);
    evio_run(loop, EVIO_RUN_DEFAULT);
    evio_timer_stop(loop, &timer);

    if (ctx.io_failed) {
        goto out;
    }
    if (!ctx.samples || ctx.samples + ctx.missed != NUM_DEADLINES || (fd_count && !ctx.io_cycles)) {
        fputs("Incomplete timer latency run\n", stderr);
        goto out;
    }

    qsort(ctx.delays, ctx.samples, sizeof(ctx.delays[0]), compare_delays);
    double p50 = (ctx.delays[(ctx.samples - 1) / 2] + ctx.delays[ctx.samples / 2]) / 2.0;
    evio_time p99 = ctx.delays[(ctx.samples * 99 + 99) / 100 - 1];
    printf("timer_latency fds=%zu samples=%zu missed=%zu io_cycles=%" PRIu64
           " p50_ns=%.1f p99_ns=%" PRIu64 "\n",
           fd_count, ctx.samples, ctx.missed, ctx.io_cycles, p50, p99);
    ok = true;

out:
    for (size_t i = 0; i < started; ++i) {
        evio_poll_stop(loop, &watchers[i]);
        close(watchers[i].fd);
    }
    evio_loop_free(loop);
    return ok;
}

int main(void)
{
    return bench_timer_latency(0) && bench_timer_latency(NUM_READY_FDS) ? EXIT_SUCCESS : EXIT_FAILURE;
}
