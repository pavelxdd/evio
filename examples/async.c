#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

#include "evio.h"

typedef struct {
    evio_loop *loop;
    evio_async *w;
} thread_data_t;

static void *thread_func(void *arg)
{
    thread_data_t *data = arg;

    printf("[Thread] Sleeping for 1 second...\n");
    sleep(1);

    printf("[Thread] Waking up the event loop.\n");
    // Thread-safe.
    evio_async_send(data->loop, data->w);

    return NULL;
}

static void async_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    printf("[Main] Async event received. Stopping loop.\n");
    evio_break(loop, EVIO_BREAK_ALL);
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    evio_async w;
    evio_async_init(&w, async_cb);

    evio_async_start(loop, &w);

    pthread_t thread;
    thread_data_t thread_data = { .loop = loop, .w = &w };
    pthread_create(&thread, NULL, thread_func, &thread_data);

    printf("[Main] Event loop running. Waiting for async event from thread.\n");

    evio_run(loop, EVIO_RUN_DEFAULT);

    pthread_join(thread, NULL);

    printf("[Main] Event loop finished.\n");

    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
