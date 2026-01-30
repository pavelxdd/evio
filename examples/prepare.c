#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

static int prepare_count = 0;

// Runs at the start of each loop iteration.
static void prepare_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    prepare_count++;
    printf("Prepare watcher called (iteration: %d).\n", prepare_count);

    if (prepare_count >= 3) {
        printf("Ran 3 times, stopping loop.\n");
        evio_break(loop, EVIO_BREAK_ALL);
    }
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    evio_prepare w;
    evio_prepare_init(&w, prepare_cb);

    evio_prepare_start(loop, &w);

    printf("Event loop running. Prepare watcher will fire on each iteration.\n");

    while (evio_run(loop, EVIO_RUN_NOWAIT)) {}

    printf("Event loop finished.\n");

    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
