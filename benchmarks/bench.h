#pragma once

#include <stdint.h>

uint64_t get_time_ns(void);

void print_benchmark(const char *category, const char *name, uint64_t time_ns, uint64_t ops);

void print_versions(void);
