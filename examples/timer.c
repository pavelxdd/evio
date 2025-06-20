#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

// A struct to hold our user data.
typedef struct {
    int count;
} timer_data;

// This callback is invoked when the timer expires.
static void timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    // The base pointer can be safely cast to the specific watcher type.
    evio_timer *w = (evio_timer *)base;

    // Retrieve our data from the watcher.
    timer_data *data = w->data;
    data->count++;
    printf("Timer fired! (count: %d)\n", data->count);

    if (data->count >= 5) {
        printf("Timer fired 5 times. Stopping timer.\n");
        // Stop this watcher. Since it's a repeating timer, we must stop it
        // explicitly. This also decrements the loop's reference count,
        // allowing the loop to exit if no other watchers are active.
        evio_timer_stop(loop, w);
    }
}

int main(void)
{
    // 1. Initialize the event loop.
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    // 2. Initialize a repeating timer.
    // It will repeat every 0.5s.
    evio_timer w;
    evio_timer_init(&w, timer_cb, EVIO_TIME_FROM_MSEC(500)); // 0.5s repeat

    // 3. Attach user data.
    timer_data data = { .count = 0 };
    w.data = &data;

    // 4. Start the watchers. This increments the loop's reference count,
    // keeping it alive. It will fire first after 1s.
    evio_timer_start(loop, &w, EVIO_TIME_FROM_SEC(1)); // 1s initial delay

    printf("Event loop running. Timer will fire every 0.5s for 5 times.\n");

    // 5. Run the event loop. It will block until there are no more active watchers.
    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished. Timer fired %d time(s).\n", data.count);

    // 6. Free the event loop and all associated resources.
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
