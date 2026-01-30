#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

typedef struct {
    int count;
} idle_data;

// Runs when the loop has no other work; the loop does not block while active.
static void idle_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
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
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    evio_idle w;
    evio_idle_init(&w, idle_cb);

    idle_data data = { .count = 0 };
    w.data = &data;

    // Keeps the loop alive.
    evio_idle_start(loop, &w);

    printf("Event loop running with only an idle watcher.\n");
    printf("It will spin without blocking, calling the idle callback repeatedly.\n\n");

    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("\nEvent loop finished.\n");

    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
