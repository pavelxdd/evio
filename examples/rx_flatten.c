#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "evio_rx.h"

#define COUNT 3

static uint64_t ids[COUNT] = { 1, 2, 3 };

// Inner run of value `id`: emits `id` after (4 - id) * 100 ms.
static evio_rx_observable *inners[COUNT];

static int replace_with_id(void *ctx, void *out, const void *value)
{
    *(uint64_t *)out = *(const uint64_t *)ctx;
    return 0;
}

typedef struct {
    const char *name;
    evio_rx_subscription *subscription;
} printer;

static evio_rx_observable *project(void *ctx, const void *value)
{
    printer *p = ctx;
    uint64_t id = *(const uint64_t *)value;
    printf("%s: start inner %" PRIu64 "\n", p->name, id);
    return evio_rx_observable_ref(inners[id - 1]);
}

static evio_rx_project projector(printer *p)
{
    return (evio_rx_project) { .type = &evio_rx_uint64_type, .cb = project, .capture.data = p };
}

static void print_next(void *ctx, const void *value)
{
    printer *p = ctx;
    printf("%s: %" PRIu64 "\n", p->name, *(const uint64_t *)value);
}

static void print_complete(void *ctx)
{
    printer *p = ctx;
    printf("%s: complete\n", p->name);
}

static void subscribe(evio_loop *loop, evio_rx_observable *source, printer *p)
{
    evio_rx_observer observer = { .next = print_next, .complete = print_complete, .ctx = p };
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

    for (size_t i = 0; i < COUNT; ++i) {
        evio_rx_observable *timer = evio_rx_timer(EVIO_TIME_FROM_MSEC(100) *(COUNT - i));
        inners[i] = evio_rx_map(timer, &evio_rx_uint64_type, replace_with_id,
                                (evio_rx_capture) { .data = &ids[i] });
        evio_rx_observable_free(timer);
    }
    evio_rx_observable *outer = evio_rx_from_array(&evio_rx_uint64_type, ids, COUNT);

    // concat: one inner at a time, in outer order: 1 2 3.
    printer concat = { .name = "concat" };
    subscribe(loop, evio_rx_concat_map(outer, projector(&concat), COUNT), &concat);

    // merge: all inners at once; the shortest timer wins: 3 2 1.
    printer merge = { .name = "merge" };
    subscribe(loop, evio_rx_merge_map(outer, projector(&merge), COUNT, COUNT), &merge);

    // switch: each outer value cancels the previous inner: only 3.
    printer switched = { .name = "switch" };
    subscribe(loop, evio_rx_switch_map(outer, projector(&switched)), &switched);

    printf("Event loop running.\n");

    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished.\n");

    evio_rx_subscription_free(concat.subscription);
    evio_rx_subscription_free(merge.subscription);
    evio_rx_subscription_free(switched.subscription);
    evio_rx_observable_free(outer);
    for (size_t i = 0; i < COUNT; ++i) {
        evio_rx_observable_free(inners[i]);
    }
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
