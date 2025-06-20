#include <stdio.h>
#include <time.h>

#include <ev.h>
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
    printf("libuv: %s\n", uv_version_string());
    printf("------------------------\n\n");
}
