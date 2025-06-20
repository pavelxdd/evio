#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

// Cleanup watchers run just before the event loop is freed.
static void cleanup_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    (void)loop;
    (void)emask;
    // Retrieve user data to be cleaned up.
    char *my_data = base->data;
    printf("Cleanup watcher called. Freeing data: '%s'\n", my_data);
    free(my_data);
}

int main(void)
{
    // 1. Initialize the event loop.
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    // 2. Initialize cleanup watcher.
    evio_cleanup w;
    evio_cleanup_init(&w, cleanup_cb);

    // 3. Allocate some data and associate it with the watcher.
    w.data = malloc(32);
    if (!w.data) {
        return EXIT_FAILURE;
    }
    sprintf(w.data, "some heap-allocated resource");

    // 4. Start the watcher. Note: cleanup watchers do not affect the loop's
    // reference count, so they don't keep the loop running on their own.
    evio_cleanup_start(loop, &w);

    printf("Cleanup watcher is active.\n");
    printf("The loop has no other active watchers, so evio_run() would exit immediately.\n");

    // We can run the loop, but it will do nothing and exit.
    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Freeing the event loop now...\n");

    // 5. Free the event loop. This will trigger the cleanup watcher's callback.
    evio_loop_free(loop);

    printf("Event loop freed.\n");
    return EXIT_SUCCESS;
}
