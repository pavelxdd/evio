#include <stdio.h>
#include <time.h>

#include <ev.h>
enum {
    LIBEV_READ  = EV_READ,
    LIBEV_WRITE = EV_WRITE,
};
#undef EV_READ
#undef EV_WRITE

#include <event2/event.h>
enum {
    LIBEVENT_READ  = EV_READ,
    LIBEVENT_WRITE = EV_WRITE,
};
#include <uv.h>

#include "evio.h"
#include "bench.h"

uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void print_benchmark(const char *category, const char *name, uint64_t time_ns, uint64_t ops)
{
    if (ops == 0) {
        return;
    }

    double ns_per_op = (double)time_ns / ops;
    // Meson benchmark format: BENCHMARK: <name>: <value> <unit>
    printf("BENCHMARK: %s_%s: %.2f ns\n", category, name, ns_per_op);
}

void print_versions(void)
{
    printf("--- Library Versions ---\n");
    printf("evio:  %u.%u.%u\n", evio_version_major(), evio_version_minor(), evio_version_patch());
    printf("libev: %d.%d\n", ev_version_major(), ev_version_minor());
    printf("libevent: %s\n", event_get_version());
    printf("libuv: %s\n", uv_version_string());
    printf("------------------------\n\n");
}
