#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "evio_rx.h"

typedef struct {
    const char *name;
    evio_rx_subscription *subscription;
    int count;
    int limit;
} printer;

static void print_next(void *ctx, const void *value)
{
    printer *p = ctx;
    printf("%s: %" PRIu64 "\n", p->name, *(const uint64_t *)value);

    if (++p->count == p->limit) {
        evio_rx_subscription_cancel(p->subscription);
    }
}

static void print_complete(void *ctx)
{
    printer *p = ctx;
    printf("%s: complete\n", p->name);
}

static void read_pipe(void *ctx, const void *value)
{
    printer *p = ctx;
    const evio_rx_poll_event *event = value;

    char buf[16];
    ssize_t n = read(event->fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("%s: read '%s'\n", p->name, buf);
    }

    // The fd stays open; the subscription only stops watching it.
    evio_rx_subscription_cancel(p->subscription);
}

static void subscribe(evio_loop *loop, evio_rx_observable *source, printer *p,
                      void (*next)(void *, const void *))
{
    evio_rx_observer observer = { .next = next, .complete = print_complete, .ctx = p };
    p->subscription = evio_rx_subscription_new(loop, source, observer, 16);
    evio_rx_subscription_start(p->subscription);
    evio_rx_observable_free(source);
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        perror("pipe");
        evio_loop_free(loop);
        return EXIT_FAILURE;
    }

    // An array is delivered synchronously, inside start.
    uint64_t items[] = { 1, 2, 3 };
    printer array = { .name = "array" };
    subscribe(loop, evio_rx_from_array(&evio_rx_uint64_type, items, 3), &array, print_next);

    printer timer = { .name = "timer" };
    subscribe(loop, evio_rx_timer(EVIO_TIME_FROM_MSEC(100)), &timer, print_next);

    // The interval never completes on its own.
    printer interval = { .name = "interval", .limit = 3 };
    subscribe(loop, evio_rx_interval(0, EVIO_TIME_FROM_MSEC(50)), &interval, print_next);

    printer poll = { .name = "poll" };
    subscribe(loop, evio_rx_from_poll(fds[0], EVIO_READ), &poll, read_pipe);
    if (write(fds[1], "hello", 5) != 5) {
        perror("write");
    }

    printf("Event loop running.\n");

    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished.\n");

    evio_rx_subscription_free(array.subscription);
    evio_rx_subscription_free(timer.subscription);
    evio_rx_subscription_free(interval.subscription);
    evio_rx_subscription_free(poll.subscription);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
