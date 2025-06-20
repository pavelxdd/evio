#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "evio.h"

// A struct to hold some user data.
typedef struct {
    const char *message;
    int run_count;
} my_loop_data;

int main(void)
{
    // 1. Initialize the event loop.
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    // 2. Associate user data with the loop.
    my_loop_data loop_data = { .message = "Hello from user data!", .run_count = 0 };
    evio_set_userdata(loop, &loop_data);

    // 3. Run the loop a few times.
    // We use EVIO_RUN_ONCE, which runs one iteration and blocks for I/O if
    // necessary (though here, it will return immediately as there are no events).
    printf("Running the loop for 3 iterations...\n");
    for (int i = 0; i < 3; ++i) {
        printf(" - Iteration %d\n", i + 1);

        // Update and get the loop's cached time.
        evio_update_time(loop);
        evio_time now = evio_get_time(loop);
        printf("   Loop time: %llu ns\n", (unsigned long long)now);

        // Retrieve and use user data.
        my_loop_data *data = evio_get_userdata(loop);
        data->run_count++;
        printf("   User data message: '%s' (run count: %d)\n", data->message, data->run_count);

        // Run one loop iteration.
        evio_run(loop, EVIO_RUN_ONCE);

        // Sleep a bit to see time change.
        usleep(10 * 1000); // 10ms
    }

    // 4. Run the loop again.
    // Since the reference count is 0, evio_run() will return immediately.
    printf("Running the loop again with refcount=0...\n");
    int active_watchers = evio_run(loop, EVIO_RUN_DEFAULT);
    printf("Loop exited immediately, active watchers: %d\n", active_watchers);

    // 5. Check that our user data was modified correctly.
    my_loop_data *data = evio_get_userdata(loop);
    printf("Final run count from user data: %d\n", data->run_count);

    // 6. Free the event loop and all associated resources.
    printf("Freeing the event loop.\n");
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
