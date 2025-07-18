# `evio` Event Loop Library

[![Build](https://img.shields.io/github/actions/workflow/status/pavelxdd/evio/meson.yml?branch=master&style=flat)](https://github.com/pavelxdd/evio/actions)
[![Codecov](https://img.shields.io/codecov/c/gh/pavelxdd/evio?style=flat)](https://codecov.io/gh/pavelxdd/evio)
[![License](https://img.shields.io/github/license/pavelxdd/evio?style=flat&color=blue)](https://github.com/pavelxdd/evio/blob/master/LICENSE)

## Overview

`evio` is a high-performance, low-level event loop library for C, designed for building scalable, event-driven network applications. It provides a rich set of features, including I/O watchers, timers, signal handlers, and inter-thread communication.

Designed and optimized specifically for **Linux**, it relies on Linux-specific APIs such as `epoll`, `eventfd`, and optionally `io_uring`. As such, it is not designed to be portable and will not compile or run on other operating systems like macOS or Windows.

## Building

The project uses Meson for building.

```bash
meson setup build
meson compile -Cbuild
```

To run tests:

```bash
meson setup build -Dtests=true
meson test -Cbuild -v
```

## API Usage Example

Here is a minimal example demonstrating a repeating timer and a signal handler.

```c
#include <stdlib.h>
#include <stdio.h>
#include <evio/evio.h>

// A struct to hold our user data.
typedef struct {
    int count;
} timer_data;

// This callback is invoked when the timer expires.
static void timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    // The base pointer can be safely cast to the specific watcher type.
    evio_timer *w = (evio_timer *)base;

    // Retrieve our data from the watcher.
    timer_data *data = w->data;
    data->count++;
    printf("Timer fired! (count: %d)\n", data->count);

    if (data->count >= 5) {
        printf("Timer fired 5 times. Stopping timer.\n");
        // Stop this watcher. Since it's a repeating timer, we must stop it
        // explicitly. This also decrements the loop's reference count,
        // allowing the loop to exit if no other watchers are active.
        evio_timer_stop(loop, w);
    }
}

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

    // 2. Initialize a repeating timer.
    // It will fire once after 1s, then repeat every 0.5s.
    evio_timer timer;
    evio_timer_init(&timer, timer_cb, EVIO_TIME_FROM_MSEC(500)); // 0.5s repeat

    // 3. Attach user data.
    timer_data data = { .count = 0 };
    timer.data = &data;

    // 4. Initialize a signal watcher to gracefully exit on Ctrl+C.
    evio_signal sig;
    evio_signal_init(&sig, signal_cb, SIGINT);

    // 5. Start the watchers. This increments the loop's reference count,
    // keeping it alive.
    evio_timer_start(loop, &timer, EVIO_TIME_FROM_SEC(1)); // 1s initial delay
    evio_signal_start(loop, &sig);

    printf("Event loop running. Timer will fire every 0.5s. Press Ctrl+C to exit.\n");

    // 6. Run the event loop. It will block until evio_break() is called
    // or there are no more active watchers.
    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished. Timer fired %d time(s).\n", data.count);

    // 7. Free the event loop and all associated resources.
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
```

## Core Concepts

The `evio` library is organized into a modular structure for clarity and maintainability, distinguishing between public headers that form the API and private headers for internal use.

At its core, the library is built around a central **event loop** that processes events from various sources. Application logic is integrated into the loop by registering **watchers**, which are handles associated with specific events and callback functions.

### The Event Loop

The `evio_loop` is the heart of the library. It continuously monitors for events and invokes the corresponding callbacks. The main functions for managing the loop are:

- `evio_loop_new(flags)` / `evio_loop_free(loop)`: Create and destroy an event loop.
- `evio_run(loop, flags)`: Start the event loop, which will run until stopped.
- `evio_break(loop, state)`: Request the event loop to stop running.
- `evio_ref(loop)` / `evio_unref(loop)`: Manually manage the loop's reference count. The loop continues to run as long as its reference count is greater than zero.
- `evio_get_time(loop)` / `evio_update_time(loop)`: Get or update the loop's cached monotonic time.
- `evio_set_userdata(loop, data)` / `evio_get_userdata(loop)`: Associate a user-defined data pointer with the loop.

### Watchers

Watchers are data structures that you initialize and register with the event loop to be notified of specific events. Each watcher has a corresponding callback function that the loop executes when the event occurs.

The main watcher types are summarized below:

| Watcher Type      | Triggers On                               | Primary Use Case                       |
|-------------------|-------------------------------------------|----------------------------------------|
| `evio_poll`       | I/O readiness on a file descriptor.       | Network sockets, pipes.                |
| `evio_timer`      | A specified time delay has passed.        | Timeouts, periodic tasks.              |
| `evio_signal`     | A POSIX signal is received.               | Graceful shutdown (e.g., on SIGINT).   |
| `evio_async`      | A manual trigger from another thread.     | Inter-thread communication.            |
| `evio_idle`       | The loop has no other pending events.     | Low-priority background tasks.         |
| `evio_prepare`    | Before the loop blocks for I/O.           | Pre-blocking setup.                    |
| `evio_check`      | After the loop has processed I/O.         | Post-I/O state updates.                |
| `evio_cleanup`    | Just before the event loop is freed.      | Guaranteed resource cleanup.           |
| `evio_once`       | The first of an I/O event or a timeout.   | I/O operations with a deadline.        |

### Event Processing Phases

The `evio` loop processes events in a well-defined order during each iteration:

1. **Prepare Watchers (`evio_prepare`)**: Callbacks are invoked right before the loop blocks to poll for I/O. This is useful for setup tasks that need to run immediately before the loop might sleep.
2. **Poll for I/O**: The loop waits for I/O events on registered file descriptors (e.g., via `epoll_pwait`). The timeout is calculated based on the nearest active timer.
3. **Process Events**: After polling, the loop processes I/O, async, and signal events, queueing their callbacks for invocation.
4. **Timer Watchers (`evio_timer`)**: The loop checks for expired timers and queues their callbacks.
5. **Idle Watchers (`evio_idle`)**: If no other events were queued in the current iteration, idle watcher callbacks are invoked. This is useful for low-priority background tasks.
6. **Invoke Callbacks**: All queued callbacks from the current iteration are executed.
7. **Check Watchers (`evio_check`)**: Callbacks are invoked immediately after the loop has processed other events. This is useful for actions that need to happen after I/O callbacks have run.
8. **Loop Termination**: The loop checks if it should continue running or exit based on its reference count and break state.
9. **Cleanup Watchers (`evio_cleanup`)**: These callbacks are invoked only once, just before the event loop is destroyed with `evio_loop_free()`.

## Performance

`evio` is designed with performance as a primary goal.

For I/O-heavy applications with a high rate of connection turnover (high "churn"), `evio` provides a significant optimization via the `EVIO_FLAG_URING` flag passed to `evio_loop_new()`. When enabled on a supported Linux system, this flag allows `evio` to use the `io_uring` interface to batch and asynchronously submit `epoll_ctl` calls, which can dramatically reduce syscall overhead compared to traditional event loops.

The `benchmarks/` directory contains a suite of performance tests that compare `evio` (both with and without the `io_uring` optimization) against other popular libraries like `libev` and `libuv`. These can be built and run using the `benchmarks` meson option.

### `io_uring` Optimization

For applications that frequently add and remove file descriptors from the event loop (e.g., high-traffic servers with short-lived connections), `evio` offers an optional optimization using Linux's `io_uring` interface.

When a loop is created with the `EVIO_FLAG_URING` flag, `evio` will use `io_uring` to batch and asynchronously submit `epoll_ctl` calls. This can significantly reduce syscall overhead and improve performance in I/O-heavy workloads.

It is important to note that this is **not** a full `io_uring` backend. The core event polling mechanism still relies on `epoll`. The optimization is specifically targeted at the management of file descriptor watchers. If `io_uring` is not available on the system when the flag is used, `evio` gracefully falls back to using standard `epoll_ctl` syscalls.

## Threading Model

The `evio` library is designed with a specific threading model in mind:

- **Single-Threaded Loop**: An `evio_loop` instance and all associated watchers are **not thread-safe**. All functions that operate on a loop (e.g., `evio_timer_start`, `evio_poll_stop`) must be called from the same thread that is running `evio_run()`.
- **Inter-Thread Communication**: To interact with a running event loop from another thread, you must use an **`evio_async`** watcher. The `evio_async_send()` function is the only function that is safe to call from a different thread. It will safely wake up the target event loop, causing the corresponding `evio_async` watcher's callback to be invoked in the loop's thread.

## Fork Safety

Like most event loop libraries, `evio` is not designed to have its state carried over a `fork()` call. Attempting to use a parent's `evio_loop` in a child process will lead to undefined behavior, as the underlying kernel state (like the `epoll` file descriptor) cannot be safely shared or repaired.

The only correct and safe way to use `evio` in a multi-process (pre-fork) server model is:

1. **Parent Process**: The parent process can create listening sockets but should **not** initialize or run an `evio_loop` that manages them. Its primary role is to `fork()` child processes.
2. **Child Process**: Each child process must create its own, new `evio_loop` instance after the `fork()` call. It can then add the inherited listening sockets and other file descriptors to this new loop.

This model ensures that each process has a completely independent and valid event loop, avoiding state corruption and unpredictable behavior. Do **not** attempt to "re-initialize" a loop in a child process. See `examples/fork.c` for a practical demonstration of this pattern.

## Error Handling

`evio` takes a pragmatic approach to error handling, distinguishing between recoverable setup errors and fatal runtime errors.

- **Recoverable Setup Errors**: Functions that depend on limited system resources, such as `evio_loop_new()`, can fail during initial setup. For example, if no more file descriptors are available, `evio_loop_new()` will return `NULL`. Applications are expected to check for and handle these `NULL` returns gracefully.

- **Unrecoverable Runtime Errors**: By design, `evio` considers certain runtime errors, such as memory allocation failures (e.g., out-of-memory), to be fatal. Instead of returning error codes for every function that might allocate memory, the library will call `EVIO_ABORT`, which by default terminates the program. This design choice simplifies the API significantly by avoiding pervasive error-checking for conditions that are rarely recoverable in practice. This behavior can be customized by providing a custom handler via `evio_set_abort()`.

## Advanced Topics

### Re-entrant Event Invocation

The `evio_invoke_pending(loop)` function, which is called internally by `evio_run()`, is re-entrant. If a watcher callback calls `evio_invoke_pending()` again, it will immediately start processing newly queued events before the original call returns. This results in a depth-first event processing order. While this can be a powerful feature for immediate, nested event handling, developers should be mindful that deep recursion can lead to stack exhaustion.

### Manual Event Injection

`evio` provides functions to manually inject events into the loop, which is useful for integrating with other event systems or for testing.

- `evio_feed_event(loop, watcher, emask)`: Queues an event for a specific watcher.
- `evio_feed_fd_event(loop, fd, emask)`: Queues an I/O event for all watchers on a given file descriptor.
- `evio_feed_fd_error(loop, fd)`: Queues an I/O error for all watchers on a given file descriptor.
- `evio_feed_signal(loop, signum)`: Simulates the delivery of a POSIX signal.

### Customization

The library includes several customization functions for convenience:

- `evio_set_allocator(cb, ctx)`: Sets a custom memory allocator for the entire library.
- `evio_set_abort(cb)`: Sets a custom handler to be called on a fatal, unrecoverable error.

## License

This project is licensed under the MIT License.
