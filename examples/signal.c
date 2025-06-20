#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

// This callback is invoked when a signal is caught.
static void signal_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    printf("\nCaught SIGINT, stopping loop.\n");
    // This will cause evio_run() to return after the current iteration.
    evio_break(loop, EVIO_BREAK_ALL);
}

int main(void)
{
    // 1. Initialize the event loop.
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    // 2. Initialize a signal watcher to gracefully exit on Ctrl+C.
    evio_signal w;
    evio_signal_init(&w, signal_cb, SIGINT);

    // 3. Start the watcher. This increments the loop's reference count,
    // keeping it alive.
    evio_signal_start(loop, &w);

    printf("Event loop running. Press Ctrl+C to exit.\n");

    // 4. Run the event loop. It will block until evio_break() is called.
    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished.\n");

    // 5. Free the event loop. The signal watcher is stopped automatically.
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
