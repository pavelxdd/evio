#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "evio_rx.h"

// Per-run producer state: counts from 1 to `last`, one value per tick.
typedef struct {
    evio_rx_emitter *emitter;
    evio_loop *loop;
    evio_timer timer;
    uint64_t value;
    uint64_t last;
} counter;

static void counter_tick(evio_rx_emitter *emitter, void *state, void *data)
{
    counter *c = state;

    c->value++;
    if (!evio_rx_emitter_next(emitter, &c->value)) {
        return;
    }
    if (c->value == c->last) {
        evio_rx_emitter_complete(emitter);
    }
}

static void counter_timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    counter *c = base->data;

    // The run may be cancelled inside; `c` is not used after this call.
    evio_rx_emitter_invoke(c->emitter, counter_tick, NULL);
}

static int counter_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    counter *c = calloc(1, sizeof(*c));
    if (!c) {
        return ENOMEM;
    }
    c->emitter = emitter;
    c->loop = loop;
    c->last = *(const uint64_t *)ctx;
    evio_timer_init(&c->timer, counter_timer_cb, EVIO_TIME_FROM_MSEC(100));
    c->timer.data = c;

    *state = c;
    return 0;
}

static void counter_start(evio_rx_emitter *emitter, void *state)
{
    counter *c = state;
    evio_timer_start(c->loop, &c->timer, EVIO_TIME_FROM_MSEC(100));
}

static void counter_stop(void *state)
{
    counter *c = state;
    evio_timer_stop(c->loop, &c->timer);
}

static void counter_destroy(void *state)
{
    printf("counter destroyed\n");
    free(state);
}

typedef struct {
    const char *name;
    evio_rx_subscription *subscription;
    uint64_t cancel_at;
} printer;

static void print_next(void *ctx, const void *value)
{
    printer *p = ctx;
    uint64_t v = *(const uint64_t *)value;
    printf("%s: %" PRIu64 "\n", p->name, v);

    if (v == p->cancel_at) {
        printf("%s: cancel\n", p->name);
        evio_rx_subscription_cancel(p->subscription);
    }
}

static void print_complete(void *ctx)
{
    printer *p = ctx;
    printf("%s: complete\n", p->name);
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    // One description; every subscription runs its own counter.
    uint64_t last = 3;
    evio_rx_source source = {
        .prepare = counter_prepare,
        .start = counter_start,
        .stop = counter_stop,
        .destroy = counter_destroy,
    };
    evio_rx_observable *counting = evio_rx_observable_new(&evio_rx_uint64_type, source,
                                                          (evio_rx_capture) { .data = &last });

    printer first = { .name = "first" };
    printer second = { .name = "second", .cancel_at = 2 };
    printer *printers[] = { &first, &second };
    size_t i = 0;
    int status = EXIT_FAILURE;
    for (; i < 2; ++i) {
        evio_rx_observer observer = {
            .next = print_next,
            .complete = print_complete,
            .ctx = printers[i],
        };
        printers[i]->subscription = evio_rx_subscription_new(loop, counting, observer, 16);
        if (!printers[i]->subscription) {
            perror("evio_rx_subscription_new");
            goto out;
        }
        evio_rx_subscription_start(printers[i]->subscription);
    }

    printf("Event loop running.\n");

    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished.\n");

    status = EXIT_SUCCESS;

out:
    while (i) {
        evio_rx_subscription_free(printers[--i]->subscription);
    }
    evio_rx_observable_free(counting);
    evio_loop_free(loop);
    return status;
}
