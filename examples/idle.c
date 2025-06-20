#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

// A struct to hold our user data.
typedef struct {
    int count;
} idle_data;

// Idle watchers run when the loop has no other events to process.
// If an idle watcher is the only active watcher, the loop will not block
// and will repeatedly call the idle callback.
static void idle_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    (void)base;
    (void)emask;

    // Retrieve our data from the watcher.
    idle_data *data = base->data;
    data->count++;
    printf("Idle watcher called (count: %d). The loop has no other work.\n", data->count);

    if (data->count >= 5) {
        printf("Idle watcher ran 5 times. Stopping loop.\n");
        evio_break(loop, EVIO_BREAK_ALL);
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

    // 2. Initialize an idle watcher.
    evio_idle w;
    evio_idle_init(&w, idle_cb);

    // 3. Attach user data.
    idle_data data = { .count = 0 };
    w.data = &data;

    // 4. Start the watcher. This increments the loop's reference count,
    // keeping it alive.
    evio_idle_start(loop, &w);

    printf("Event loop running with only an idle watcher.\n");
    printf("It will spin without blocking, calling the idle callback repeatedly.\n\n");

    // 5. Run the event loop.
    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("\nEvent loop finished.\n");

    // 6. Free the event loop.
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
