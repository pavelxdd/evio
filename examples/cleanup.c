#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

// Runs from evio_loop_free().
static void cleanup_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    char *my_data = base->data;
    printf("Cleanup watcher called. Freeing data: '%s'\n", my_data);
    free(my_data);
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    evio_cleanup w;
    evio_cleanup_init(&w, cleanup_cb);

    w.data = malloc(32);
    if (!w.data) {
        return EXIT_FAILURE;
    }
    sprintf(w.data, "some heap-allocated resource");

    // Cleanup watchers do not affect refcount.
    evio_cleanup_start(loop, &w);

    printf("Cleanup watcher is active.\n");
    printf("The loop has no other active watchers, so evio_run() would exit immediately.\n");

    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Freeing the event loop now...\n");

    evio_loop_free(loop);

    printf("Event loop freed.\n");
    return EXIT_SUCCESS;
}
