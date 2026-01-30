#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

typedef struct {
    int count;
} timer_data;

static void timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_timer *w = (evio_timer *)base;

    timer_data *data = w->data;
    data->count++;
    printf("Timer fired! (count: %d)\n", data->count);

    if (data->count >= 5) {
        printf("Timer fired 5 times. Stopping timer.\n");
        evio_timer_stop(loop, w);
    }
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    evio_timer w;
    evio_timer_init(&w, timer_cb, EVIO_TIME_FROM_MSEC(500)); // 0.5s repeat

    timer_data data = { .count = 0 };
    w.data = &data;

    evio_timer_start(loop, &w, EVIO_TIME_FROM_SEC(1)); // 1s initial delay

    printf("Event loop running. Timer will fire every 0.5s for 5 times.\n");

    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished. Timer fired %d time(s).\n", data.count);

    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
