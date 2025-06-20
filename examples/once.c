#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "evio.h"

// This is the callback for our evio_once watcher.
static void once_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    if (emask & EVIO_TIMER) {
        printf("Once watcher triggered by TIMEOUT.\n");
    }

    if (emask & EVIO_READ) {
        printf("Once watcher triggered by I/O (read event).\n");
    }

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

    // --- Part 1: Trigger by I/O ---
    printf("--- Part 1: Trigger by I/O ---\n");
    int fds1[2] = { -1, -1 };
    if (pipe(fds1) != 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    // 2. Initialize a once watcher with a long timeout.
    evio_once once1;
    evio_once_init(&once1, once_cb, fds1[0], EVIO_READ);
    evio_once_start(loop, &once1, EVIO_TIME_FROM_SEC(5)); // 5s timeout

    // 3. Immediately write to the pipe to trigger the I/O event.
    printf("Writing to pipe to trigger I/O event...\n");
    write(fds1[1], "a", 1);

    // 4. Run the loop. The I/O event will fire before the timeout.
    evio_run(loop, EVIO_RUN_DEFAULT);
    close(fds1[0]);
    close(fds1[1]);

    // --- Part 2: Trigger by Timeout ---
    printf("\n--- Part 2: Trigger by Timeout ---\n");
    int fds2[2];
    if (pipe(fds2) != 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    // Reset the loop's break state so we can run it again.
    evio_break(loop, EVIO_BREAK_CANCEL);

    // 5. Initialize another once watcher with a short timeout.
    evio_once once2;
    evio_once_init(&once2, once_cb, fds2[0], EVIO_READ);
    evio_once_start(loop, &once2, EVIO_TIME_FROM_SEC(1)); // 1s timeout

    printf("Waiting for 1s timeout...\n");
    // We do NOT write to the pipe this time.

    // 6. Run the loop. The timeout will fire.
    evio_run(loop, EVIO_RUN_DEFAULT);
    close(fds2[0]);
    close(fds2[1]);

    printf("\nEvent loop finished.\n");
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
