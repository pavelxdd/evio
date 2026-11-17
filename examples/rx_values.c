#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "evio_rx.h"

static bool same(void *ctx, const void *previous, const void *value)
{
    return *(const uint64_t *)previous == *(const uint64_t *)value;
}

static bool is_odd(void *ctx, const void *value)
{
    return *(const uint64_t *)value & 1;
}

static int times_ten(void *ctx, void *out, const void *value)
{
    *(uint64_t *)out = *(const uint64_t *)value * 10;
    return 0;
}

static int sum(void *ctx, void *out, const void *accumulator, const void *value)
{
    *(uint64_t *)out = *(const uint64_t *)accumulator + *(const uint64_t *)value;
    return 0;
}

static void print_number(void *ctx, const void *value)
{
    printf("sum: %" PRIu64 "\n", *(const uint64_t *)value);
}

// Batch callbacks handle a whole array of values per call.
static int odd_batch(void *ctx, void *out, size_t *kept, const void *values, size_t count)
{
    const uint64_t *value = values;
    uint64_t *result = out;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        if (value[i] & 1) {
            result[n++] = value[i];
        }
    }
    *kept = n;
    return 0;
}

static int times_ten_batch(void *ctx, void *out, const void *values, size_t count)
{
    const uint64_t *value = values;
    uint64_t *result = out;
    for (size_t i = 0; i < count; ++i) {
        result[i] = value[i] * 10;
    }
    return 0;
}

static void print_batch(void *ctx, const void *value)
{
    printf("batch: %" PRIu64 "\n", *(const uint64_t *)value);
}

// A value that owns heap memory: Rx copies it with `copy` and frees it with `destroy`.
static int string_copy(void *ctx, void *dst, const void *src)
{
    char *copy = strdup(*(char *const *)src);
    if (!copy) {
        return ENOMEM;
    }
    *(char **)dst = copy;
    return 0;
}

static void string_destroy(void *ctx, void *value)
{
    free(*(char **)value);
}

static const evio_rx_type string_type = {
    .size = sizeof(char *),
    .alignment = alignof(char *),
    .copy = string_copy,
    .destroy = string_destroy,
};

static void print_string(void *ctx, const void *value)
{
    printf("word: %s\n", *(char *const *)value);
}

static void run(evio_loop *loop, evio_rx_observable *source, evio_rx_observer observer)
{
    evio_rx_subscription *subscription = evio_rx_subscription_new(loop, source, observer, 16);
    evio_rx_subscription_start(subscription);
    evio_rx_subscription_free(subscription);
}

int main(void)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop.\n");
        return EXIT_FAILURE;
    }

    // 1 1 2 3 3 -> distinct 1 2 3 -> odd 1 3 -> x10 10 30 -> running sum 10 40.
    uint64_t items[] = { 1, 1, 2, 3, 3 };
    uint64_t zero = 0;
    evio_rx_observable *array = evio_rx_from_array(&evio_rx_uint64_type, items, 5);
    evio_rx_observable *distinct = evio_rx_distinct_until_changed(array, same,
                                                                  (evio_rx_capture) {0});
    evio_rx_observable *odd = evio_rx_filter(distinct, is_odd, (evio_rx_capture) {0});
    evio_rx_observable *scaled = evio_rx_map(odd, &evio_rx_uint64_type, times_ten,
                                             (evio_rx_capture) {0});
    evio_rx_observable *total = evio_rx_scan(scaled, &evio_rx_uint64_type, &zero, sum,
                                             (evio_rx_capture) {0});

    run(loop, total, (evio_rx_observer) { .next = print_number });

    // Each operator holds its input; only the last handle is needed.
    evio_rx_observable_free(array);
    evio_rx_observable_free(distinct);
    evio_rx_observable_free(odd);
    evio_rx_observable_free(scaled);
    evio_rx_observable_free(total);

    char first[] = "alpha";
    char second[] = "beta";
    char *words[] = { first, second };
    evio_rx_observable *strings = evio_rx_from_array(&string_type, words, 2);
    if (!strings) {
        perror("evio_rx_from_array");
        evio_loop_free(loop);
        return EXIT_FAILURE;
    }

    // The array keeps its own copies: this change is not seen by subscribers.
    first[0] = 'A';

    run(loop, strings, (evio_rx_observer) { .next = print_string });
    evio_rx_observable_free(strings);

    // 1 2 3 4 5 published at once -> odd 1 3 5 -> x10 10 30 50.
    evio_rx_subject *subject = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
    evio_rx_observable *published = evio_rx_from_subject(subject);
    evio_rx_observable *odd_values = evio_rx_filter_batch(published, odd_batch,
                                                          (evio_rx_capture) {0});
    evio_rx_observable *tens = evio_rx_map_batch(odd_values, &evio_rx_uint64_type,
                                                 times_ten_batch, (evio_rx_capture) {0});
    evio_rx_subscription *subscription =
        evio_rx_subscription_new(loop, tens, (evio_rx_observer) { .next = print_batch }, 16);
    evio_rx_subscription_start(subscription);
    uint64_t batch[] = { 1, 2, 3, 4, 5 };
    evio_rx_subject_next_batch(subject, batch, 5);
    evio_rx_subscription_free(subscription);
    evio_rx_observable_free(published);
    evio_rx_observable_free(odd_values);
    evio_rx_observable_free(tens);
    evio_rx_subject_free(subject);

    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
