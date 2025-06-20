#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "evio.h"

// This callback is invoked when the pipe becomes readable.
static void poll_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    (void)emask;
    evio_poll *w = (evio_poll *)base;
    char buf[16];

    // Read the data from the pipe.
    ssize_t n = read(w->fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("Poll watcher triggered. Read '%s' from pipe.\n", buf);
    }

    // Stop the poll watcher. Since it's the only active watcher with a
    // reference count, the loop will exit.
    printf("Stopping poll watcher.\n");
    evio_poll_stop(loop, w);
}

int main(void)
{
    // 1. Initialize the event loop.
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    // 2. Create a pipe. We'll watch the read end and write to the write end.
    int fds[2] = { -1, -1 };
    if (pipe(fds) != 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }
    int read_fd = fds[0];
    int write_fd = fds[1];

    // 3. Initialize and start a poll watcher for the read end of the pipe.
    evio_poll w;
    evio_poll_init(&w, poll_cb, read_fd, EVIO_READ);
    evio_poll_start(loop, &w);

    // 4. Write data to the pipe from the main thread.
    // This will make the read_fd readable and trigger the poll watcher
    // in the next loop iteration.
    printf("Writing to pipe to make it readable...\n");
    write(write_fd, "hello", 5);

    printf("Event loop running. Poll event is ready.\n");

    // 5. Run the event loop. It will process the ready I/O event and call
    // the poll_cb, which will then stop the watcher, causing the loop to exit.
    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished.\n");

    // 6. Clean up.
    close(read_fd);
    close(write_fd);
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
