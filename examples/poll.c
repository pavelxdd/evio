#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "evio.h"

static void poll_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_poll *w = (evio_poll *)base;
    char buf[16];

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
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    // Watch read end; write from main.
    int fds[2] = { -1, -1 };
    if (pipe(fds) != 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }
    int read_fd = fds[0];
    int write_fd = fds[1];

    evio_poll w;
    evio_poll_init(&w, poll_cb, read_fd, EVIO_READ);
    evio_poll_start(loop, &w);

    printf("Writing to pipe to make it readable...\n");
    write(write_fd, "hello", 5);

    printf("Event loop running. Poll event is ready.\n");

    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished.\n");

    close(read_fd);
    close(write_fd);
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
