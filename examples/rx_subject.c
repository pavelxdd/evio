#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "evio_rx.h"

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
    return subscription;
}

static void publish(evio_rx_subject *subject, uint64_t value)
{
    printf("publish %" PRIu64 "\n", value);
    if (evio_rx_subject_next(subject, &value) != 0) {
        perror("evio_rx_subject_next");
    }
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    // A Subject is hot: subscribers see only what is published after they start.
    evio_rx_subject *subject = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
    evio_rx_observable *source = evio_rx_from_subject(subject);

    evio_rx_subscription *a = subscribe(loop, source, "a");
    publish(subject, 1);

    evio_rx_subscription *b = subscribe(loop, source, "b");
    publish(subject, 2);

    printf("cancel a\n");
    evio_rx_subscription_cancel(a);
    publish(subject, 3);

    evio_rx_subject_complete(subject);

    evio_rx_subscription_free(a);
    evio_rx_subscription_free(b);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
