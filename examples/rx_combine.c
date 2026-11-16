#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "evio_rx.h"

static int sum(void *ctx, void *out, const void *const *values, size_t count)
{
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        total += *(const uint64_t *)values[i];
    }
    *(uint64_t *)out = total;
    return 0;
}

static void print_next(void *ctx, const void *value)
{
    printf("%s: %" PRIu64 "\n", (const char *)ctx, *(const uint64_t *)value);
}

static void print_complete(void *ctx)
{
    printf("%s: complete\n", (const char *)ctx);
}

static evio_rx_subscription *subscribe(evio_loop *loop, evio_rx_observable *source,
                                       const char *name)
{
    evio_rx_observer observer = {
        .next = print_next,
        .complete = print_complete,
        .ctx = (void *)name,
    };
    evio_rx_subscription *subscription = evio_rx_subscription_new(loop, source, observer, 16);
    evio_rx_subscription_start(subscription);
    evio_rx_observable_free(source);
    return subscription;
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    uint64_t ones[] = { 1, 2 };
    uint64_t tens[] = { 10, 20 };
    evio_rx_observable *arrays[] = {
        evio_rx_from_array(&evio_rx_uint64_type, ones, 2),
        evio_rx_from_array(&evio_rx_uint64_type, tens, 2),
    };
    evio_rx_combiner adder = { .type = &evio_rx_uint64_type, .cb = sum };

    // Arrays are synchronous, so merge forwards the first one entirely: 1 2 10 20.
    evio_rx_subscription_free(subscribe(loop, evio_rx_merge(arrays, 2), "merge"));

    // Zip pairs values by position: 1+10, 2+20.
    evio_rx_subscription_free(subscribe(loop, evio_rx_zip(arrays, 2, adder, 2), "zip"));

    evio_rx_observable_free(arrays[0]);
    evio_rx_observable_free(arrays[1]);

    // combine_latest emits on every change once each input has a value.
    evio_rx_subject *a = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
    evio_rx_subject *b = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
    evio_rx_observable *subjects[] = { evio_rx_from_subject(a), evio_rx_from_subject(b) };
    evio_rx_subscription *latest = subscribe(loop, evio_rx_combine_latest(subjects, 2, adder),
                                             "latest");

    uint64_t values[] = { 1, 10, 2 };
    evio_rx_subject_next(a, &values[0]);
    evio_rx_subject_next(b, &values[1]);
    evio_rx_subject_next(a, &values[2]);
    evio_rx_subject_complete(a);
    evio_rx_subject_complete(b);

    evio_rx_subscription_free(latest);
    evio_rx_observable_free(subjects[0]);
    evio_rx_observable_free(subjects[1]);
    evio_rx_subject_free(a);
    evio_rx_subject_free(b);
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
