# `evio` Event Loop Library

[![Build](https://img.shields.io/github/actions/workflow/status/pavelxdd/evio/meson.yml?branch=master&style=flat)](https://github.com/pavelxdd/evio/actions)
[![Codecov](https://img.shields.io/codecov/c/gh/pavelxdd/evio?style=flat)](https://codecov.io/gh/pavelxdd/evio)
[![License](https://img.shields.io/github/license/pavelxdd/evio?style=flat&color=blue)](https://github.com/pavelxdd/evio/blob/master/LICENSE)

## Overview

`evio` is a low-level event loop library for C, built for Linux. It relies on `epoll` and `eventfd` and is not intended to be portable.

`EVIO_FLAG_URING` enables an `io_uring` fast path for poll watcher churn (the loop still waits via `epoll`).

## Building

just:
```bash
just test          # Linux host
just docker-test   # macOS/Windows host
```

build:
```bash
meson setup build
meson compile -Cbuild
```

tests:
```bash
meson setup build -Dtests=true
meson test -Cbuild -v
```

docker:
```bash
docker compose build --pull evio
docker compose run --rm evio bash -lc 'meson setup build -Dtests=true && meson test -Cbuild -v'
```

podman:
```bash
podman-compose build --pull evio
podman-compose run --rm evio bash -lc 'meson setup build -Dtests=true && meson test -Cbuild -v'
```

## API Usage Example

Minimal repeating timer + SIGINT handler.

```c
#include <stdlib.h>
#include <stdio.h>
#include <evio/evio.h>

typedef struct {
    int count;
} timer_data;

static void timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    (void)emask;

    evio_timer *w = (evio_timer *)base;
    timer_data *data = w->data;

    data->count++;
    printf("Timer fired! (count: %d)\n", data->count);

    if (data->count >= 5) {
        evio_timer_stop(loop, w);
    }
}

static void signal_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    (void)base;
    (void)emask;

    evio_break(loop, EVIO_BREAK_ALL);
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        return EXIT_FAILURE;
    }

    evio_timer timer;
    evio_timer_init(&timer, timer_cb, EVIO_TIME_FROM_MSEC(500)); // 0.5s repeat

    timer_data data = { .count = 0 };
    timer.data = &data;

    evio_signal sig;
    evio_signal_init(&sig, signal_cb, SIGINT);

    evio_timer_start(loop, &timer, EVIO_TIME_FROM_SEC(1)); // 1s initial delay
    evio_signal_start(loop, &sig);

    evio_run(loop, EVIO_RUN_DEFAULT);

    printf("Event loop finished. Timer fired %d time(s).\n", data.count);
    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
```

## Notes

- Linux-only (`epoll`, `eventfd`).
- Allocator: can be customized via `evio_set_allocator()`.
- Threading: `evio_loop` and watchers are single-threaded; cross-thread wakeups via `evio_async_send()`.
- Fork: create a new loop after `fork()` in the child.
- Fatal errors: unrecoverable conditions call `EVIO_ABORT` (customizable via `evio_set_abort()`).
- `evio_invoke_pending()` is re-entrant; avoid unbounded recursion from callbacks.

## License

This project is licensed under the MIT License.
