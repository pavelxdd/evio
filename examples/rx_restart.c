#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "evio_rx.h"

static int add(void *ctx, void *out, const void *accumulator, const void *value)
{
    *(uint64_t *)out = *(const uint64_t *)accumulator + *(const uint64_t *)value;
    return 0;
}

static void print_next(void *ctx, const void *value)
{
    printf("sum: %" PRIu64 "\n", *(const uint64_t *)value);
}

static void print_complete(void *ctx)
{
    printf("done\n");
}

static void run_task(evio_rx_subject *subject, evio_rx_subscription *subscription,
                     uint64_t first, uint64_t second)
{
    // The Subject and the subscription closed with the previous task.
    if (evio_rx_subject_reopen(subject) != 0) {
        perror("evio_rx_subject_reopen");
        return;
    }
    evio_rx_subscription_restart(subscription);

    evio_rx_subject_next(subject, &first);
    evio_rx_subject_next(subject, &second);
    evio_rx_subject_complete(subject);
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    evio_rx_subject *subject = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    uint64_t zero = 0;
    evio_rx_observable *sum = evio_rx_scan(source, &evio_rx_uint64_type, &zero, add,
                                           (evio_rx_capture) {0});
    evio_rx_observable_free(source);

    evio_rx_observer observer = {
        .next = print_next,
        .complete = print_complete,
    };
    evio_rx_subscription *subscription = evio_rx_subscription_new(loop, sum, observer, 16);

    // Each task reuses the same memory; the sum starts over: 1 3, then 10 30.
    run_task(subject, subscription, 1, 2);
    run_task(subject, subscription, 10, 20);

    evio_rx_subscription_free(subscription);
    evio_rx_observable_free(sum);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
