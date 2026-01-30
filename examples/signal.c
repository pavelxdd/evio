#include <stdlib.h>
#include <stdio.h>

#include "evio.h"

static void signal_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    printf("\nCaught SIGINT, stopping loop.\n");
    evio_break(loop, EVIO_BREAK_ALL);
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    evio_signal w;
    evio_signal_init(&w, signal_cb, SIGINT);

    evio_signal_start(loop, &w);

    printf("Event loop running. Press Ctrl+C to exit.\n");

    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished.\n");

    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
