#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

static int check_count = 0;

// Check watchers run at the end of each loop iteration, after all other
// events for that iteration have been processed.
static void check_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    check_count++;
    printf("Check watcher called (iteration: %d).\n", check_count);

    if (check_count >= 3) {
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

    // 2. Initialize a check watcher.
    evio_check w;
    evio_check_init(&w, check_cb);

    // 3. Start the watcher. This keeps the loop running.
    evio_check_start(loop, &w);

    printf("Event loop running. Check watcher will fire on each iteration.\n");

    // 4. Run the event loop.
    while (evio_run(loop, EVIO_RUN_NOWAIT)) {}

    printf("Event loop finished.\n");

    // 5. Free the event loop.
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
