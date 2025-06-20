#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include "evio.h"

// A struct to pass to the new thread.
typedef struct {
    evio_loop *loop;
    evio_async *w;
} thread_data_t;

// This function runs in a separate thread.
static void *thread_func(void *arg)
{
    thread_data_t *data = arg;

    printf("[Thread] Sleeping for 1 second...\n");
    sleep(1);

    printf("[Thread] Waking up the event loop.\n");
    // This is the only evio function that is safe to call from another thread.
    evio_async_send(data->loop, data->w);

    return NULL;
}

// This callback is invoked in the main loop's thread when the async event is received.
static void async_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    (void)base;
    (void)emask;
    printf("[Main] Async event received. Stopping loop.\n");
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

    // 2. Initialize an async watcher.
    evio_async w;
    evio_async_init(&w, async_cb);

    // 3. Start the async watcher.
    evio_async_start(loop, &w);

    // 4. Create a new thread to send the async signal.
    pthread_t thread;
    thread_data_t thread_data = { .loop = loop, .w = &w };

    int err = pthread_create(&thread, NULL, thread_func, &thread_data);
    if (err != 0) {
        errno = err;
        perror("pthread_create");
        return EXIT_FAILURE;
    }

    printf("[Main] Event loop running. Waiting for async event from thread.\n");

    // 5. Run the event loop.
    evio_run(loop, EVIO_RUN_DEFAULT);

    // 6. Wait for the other thread to finish.
    pthread_join(thread, NULL);

    printf("[Main] Event loop finished.\n");

    // 7. Free the event loop.
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
