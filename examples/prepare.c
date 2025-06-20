#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

static int prepare_count = 0;

// Prepare watchers run at the very beginning of each loop iteration,
// right before the loop blocks to wait for I/O events.
static void prepare_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    (void)base;
    (void)emask;
    prepare_count++;
    printf("Prepare watcher called (iteration: %d).\n", prepare_count);

    if (prepare_count >= 3) {
        printf("Ran 3 times, stopping loop.\n");
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

    // 2. Initialize a prepare watcher.
    evio_prepare w;
    evio_prepare_init(&w, prepare_cb);

    // 3. Start the watcher. This keeps the loop running.
    evio_prepare_start(loop, &w);

    printf("Event loop running. Prepare watcher will fire on each iteration.\n");

    // 4. Run the event loop.
    while (evio_run(loop, EVIO_RUN_NOWAIT)) {}

    printf("Event loop finished.\n");

    // 5. Free the event loop.
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
