#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "evio_rx.h"

typedef struct {
    const char *name;
    evio_loop *loop;
    evio_time start;
    evio_rx_subscription *subscription;
} printer;

static uint64_t elapsed_ms(const printer *p)
{
    return (evio_get_time(p->loop) - p->start) / EVIO_TIME_FROM_MSEC(1);
}

static void print_next(void *ctx, const void *value)
{
    printer *p = ctx;
    printf("[%3" PRIu64 " ms] %s: %" PRIu64 "\n", elapsed_ms(p), p->name,
           *(const uint64_t *)value);
}

static void print_error(void *ctx, int error)
{
    printer *p = ctx;
    printf("[%3" PRIu64 " ms] %s: %s\n", elapsed_ms(p), p->name, strerror(error));
}

static void print_complete(void *ctx)
{
    printer *p = ctx;
    printf("[%3" PRIu64 " ms] %s: complete\n", elapsed_ms(p), p->name);
}

static void subscribe(evio_loop *loop, evio_rx_observable *source, printer *p)
{
    evio_rx_observer observer = {
        .next = print_next,
        .error = print_error,
        .complete = print_complete,
        .ctx = p,
    };
    p->loop = loop;
    p->start = evio_get_time(loop);
    p->subscription = evio_rx_subscription_new(loop, source, observer, 16);
    evio_rx_subscription_start(p->subscription);
    evio_rx_observable_free(source);
}

static void complete_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_rx_subject_complete(base->data);
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    evio_rx_subject *subject = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
    evio_rx_observable *input = evio_rx_from_subject(subject);
    evio_time window = EVIO_TIME_FROM_MSEC(100);

    printer throttle = { .name = "throttle" };
    printer debounce = { .name = "debounce" };
    printer delay = { .name = "delay" };
    subscribe(loop, evio_rx_throttle_time(input, window), &throttle);
    subscribe(loop, evio_rx_debounce_time(input, window), &debounce);
    subscribe(loop, evio_rx_delay(input, EVIO_TIME_FROM_MSEC(50), 16), &delay);

    // A silent input fails the timeout.
    evio_rx_subject *silent = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
    evio_rx_observable *never = evio_rx_from_subject(silent);
    printer timeout = { .name = "timeout" };
    subscribe(loop, evio_rx_timeout(never, EVIO_TIME_FROM_MSEC(200)), &timeout);

    // A burst of 1 2 3 at once, then complete after 300 ms.
    for (uint64_t value = 1; value <= 3; ++value) {
        evio_rx_subject_next(subject, &value);
    }
    evio_timer done;
    evio_timer_init(&done, complete_cb, 0);
    done.data = subject;
    evio_timer_start(loop, &done, EVIO_TIME_FROM_MSEC(300));

    printf("Event loop running.\n");

    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished.\n");

    evio_rx_subscription_free(throttle.subscription);
    evio_rx_subscription_free(debounce.subscription);
    evio_rx_subscription_free(delay.subscription);
    evio_rx_subscription_free(timeout.subscription);
    evio_rx_observable_free(input);
    evio_rx_observable_free(never);
    evio_rx_subject_free(subject);
    evio_rx_subject_free(silent);
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
