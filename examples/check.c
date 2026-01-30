#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

static int check_count = 0;

// Runs at the end of each loop iteration.
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
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    evio_check w;
    evio_check_init(&w, check_cb);

    evio_check_start(loop, &w);

    printf("Event loop running. Check watcher will fire on each iteration.\n");

    while (evio_run(loop, EVIO_RUN_NOWAIT)) {}

    printf("Event loop finished.\n");

    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
