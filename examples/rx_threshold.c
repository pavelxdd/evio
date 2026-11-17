#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evio_rx.h"

typedef struct {
    uint64_t limit;
    evio_rx_subscription *subscription;
    int error;
} threshold_monitor;

static const evio_rx_type bool_type = {
    .size = sizeof(bool),
    .alignment = alignof(bool),
};

static int above_limit(void *ctx, void *out, const void *value)
{
    threshold_monitor *monitor = ctx;
    *(bool *)out = *(const uint64_t *)value > monitor->limit;
    return 0;
}

static bool same_state(void *ctx, const void *previous, const void *value)
{
    return *(const bool *)previous == *(const bool *)value;
}

static void print_state(void *ctx, const void *value)
{
    puts(*(const bool *)value ? "above limit" : "within limit");
}

static void monitor_error(void *ctx, int error)
{
    threshold_monitor *monitor = ctx;
    monitor->error = error;
    fprintf(stderr, "threshold subscription: %s\n", strerror(error));
}

static int monitor_start(threshold_monitor *monitor, evio_loop *loop, evio_rx_subject *samples)
{
    evio_rx_observable *source = evio_rx_from_subject(samples);
    if (!source) {
        return errno;
    }
    int error = 0;
    evio_rx_observable *state = evio_rx_map(source, &bool_type, above_limit,
                                            (evio_rx_capture) { .data = monitor });
    if (!state) {
        error = errno;
        goto out_source;
    }
    evio_rx_observable *changes = evio_rx_distinct_until_changed(state, same_state,
                                                                 (evio_rx_capture) {0});
    if (!changes) {
        error = errno;
        goto out_state;
    }

    evio_rx_observer observer = { .next = print_state, .error = monitor_error, .ctx = monitor };
    monitor->subscription = evio_rx_subscription_new(loop, changes, observer, 1);
    if (!monitor->subscription) {
        error = errno;
    } else {
        evio_rx_subscription_start(monitor->subscription);
    }
    evio_rx_observable_free(changes);
out_state:
    evio_rx_observable_free(state);
out_source:
    evio_rx_observable_free(source);
    return error;
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        perror("evio_loop_new");
        return EXIT_FAILURE;
    }
    int status = EXIT_FAILURE;
    evio_rx_subject *samples = evio_rx_subject_new(loop, &evio_rx_uint64_type, 1);
    if (!samples) {
        perror("evio_rx_subject_new");
        goto out_loop;
    }
    threshold_monitor monitor = { .limit = 100 };
    int error = monitor_start(&monitor, loop, samples);
    if (error) {
        fprintf(stderr, "monitor_start: %s\n", strerror(error));
        goto out_subject;
    }

    uint64_t values[] = { 80, 110, 115, 90 };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (evio_rx_subject_next(samples, &values[i]) < 0) {
            perror("evio_rx_subject_next");
            goto out_subscription;
        }
        if (monitor.error) {
            goto out_subscription;
        }
    }
    status = EXIT_SUCCESS;

out_subscription:
    evio_rx_subscription_free(monitor.subscription);
out_subject:
    evio_rx_subject_free(samples);
out_loop:
    evio_loop_free(loop);
    return status;
}
