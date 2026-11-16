#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "evio_rx.h"

// Search as you type: queries are debounced, every new query cancels the
// request in flight, and the first response stops the whole pipeline.

typedef struct {
    evio_rx_subject *queries;
    evio_rx_subject *stop;
} search;

static bool not_empty(void *ctx, const void *value)
{
    return *(const uint64_t *)value != 0;
}

static int reply(void *ctx, void *out, const void *value)
{
    *(uint64_t *)out = *(const uint64_t *)ctx;
    return 0;
}

// A request is a timer that answers with its query after 200 ms.
static evio_rx_observable *send_request(void *ctx, const void *value)
{
    uint64_t *query = malloc(sizeof(*query));
    if (!query) {
        errno = ENOMEM;
        return NULL;
    }
    *query = *(const uint64_t *)value;
    printf("request %" PRIu64 ": sent\n", *query);

    evio_rx_observable *timer = evio_rx_timer(EVIO_TIME_FROM_MSEC(200));
    evio_rx_observable *request = evio_rx_map(timer, &evio_rx_uint64_type, reply,
                                              (evio_rx_capture) { .data = query, .destroy = free });
    evio_rx_observable_free(timer);
    return request;
}

static void on_response(void *ctx, const void *value)
{
    search *s = ctx;
    printf("response %" PRIu64 "\n", *(const uint64_t *)value);

    uint64_t signal = 1;
    evio_rx_subject_next(s->stop, &signal);
}

static void on_complete(void *ctx)
{
    printf("complete\n");
}

static void enter_query(search *s, uint64_t query)
{
    printf("query %" PRIu64 "\n", query);
    evio_rx_subject_next(s->queries, &query);
}

static void type_later_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    enter_query(base->data, 2);
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    search s = {
        .queries = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16),
        .stop = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16),
    };

    // queries -> filter -> debounce -> switch_map(request) -> take_until(stop)
    evio_rx_observable *queries = evio_rx_from_subject(s.queries);
    evio_rx_observable *stop = evio_rx_from_subject(s.stop);
    evio_rx_observable *typed = evio_rx_filter(queries, not_empty, (evio_rx_capture) {0});
    evio_rx_observable *settled = evio_rx_debounce_time(typed, EVIO_TIME_FROM_MSEC(50));
    evio_rx_project request = { .type = &evio_rx_uint64_type, .cb = send_request };
    evio_rx_observable *responses = evio_rx_switch_map(settled, request);
    evio_rx_observable *results = evio_rx_take_until(responses, stop);

    evio_rx_observer observer = { .next = on_response, .complete = on_complete, .ctx = &s };
    evio_rx_subscription *subscription = evio_rx_subscription_new(loop, results, observer, 16);
    evio_rx_subscription_start(subscription);

    // Query 1 is sent at 50 ms and cancelled by query 2, sent at 150 ms.
    enter_query(&s, 0);
    enter_query(&s, 1);
    evio_timer later;
    evio_timer_init(&later, type_later_cb, 0);
    later.data = &s;
    evio_timer_start(loop, &later, EVIO_TIME_FROM_MSEC(100));

    printf("Event loop running.\n");

    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished.\n");

    evio_rx_subscription_free(subscription);
    evio_rx_observable_free(queries);
    evio_rx_observable_free(stop);
    evio_rx_observable_free(typed);
    evio_rx_observable_free(settled);
    evio_rx_observable_free(responses);
    evio_rx_observable_free(results);
    evio_rx_subject_free(s.queries);
    evio_rx_subject_free(s.stop);
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
