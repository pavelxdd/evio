#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "evio.h"

typedef struct {
    const char *message;
    int run_count;
} my_loop_data;

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    my_loop_data loop_data = { .message = "Hello from user data!", .run_count = 0 };
    evio_set_userdata(loop, &loop_data);

    // EVIO_RUN_ONCE runs one iteration (blocks if needed).
    printf("Running the loop for 3 iterations...\n");
    for (int i = 0; i < 3; ++i) {
        printf(" - Iteration %d\n", i + 1);

        evio_update_time(loop);
        evio_time now = evio_get_time(loop);
        printf("   Loop time: %llu ns\n", (unsigned long long)now);

        my_loop_data *data = evio_get_userdata(loop);
        data->run_count++;
        printf("   User data message: '%s' (run count: %d)\n", data->message, data->run_count);

        evio_run(loop, EVIO_RUN_ONCE);

        usleep(10 * 1000); // 10ms
    }

    printf("Running the loop again with refcount=0...\n");
    int active_watchers = evio_run(loop, EVIO_RUN_DEFAULT);
    printf("Loop exited immediately, active watchers: %d\n", active_watchers);

    my_loop_data *data = evio_get_userdata(loop);
    printf("Final run count from user data: %d\n", data->run_count);

    printf("Freeing the event loop.\n");
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
