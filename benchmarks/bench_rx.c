#include <stdlib.h>

#include "evio.h"
#include "evio_rx.h"
#include "bench.h"

#define NUM_VALUES 1000000
#define NUM_PUBLISH 200000
#define NUM_CYCLES 100000
#define NUM_TIMERS 10000
#define NUM_SUBSCRIBERS 8

typedef struct {
    uint64_t count;
    uint64_t sum;
} sink;

// Out of line, so native loops pay one callback per delivery like Rx observers.
[[gnu::noinline]]
static void sink_next(void *ctx, const void *value)
{
    sink *s = ctx;
    s->count++;
    s->sum += *(const uint64_t *)value;
}

[[gnu::noinline]]
static void sink_complete(void *ctx)
{
    sink *s = ctx;
    s->count++;
}

static evio_rx_observer sink_observer(sink *s)
{
    return (evio_rx_observer) { .next = sink_next, .complete = sink_complete, .ctx = s };
}

static void check(const sink *s, uint64_t count, uint64_t sum)
{
    if (s->count != count || s->sum != sum) {
        abort();
    }
}

static int add_two(void *ctx, void *out, const void *value)
{
    *(uint64_t *)out = *(const uint64_t *)value + 2;
    return 0;
}

static bool is_even(void *ctx, const void *value)
{
    return !(*(const uint64_t *)value & 1);
}

static int add_two_batch(void *ctx, void *out, const void *values, size_t count)
{
    uint64_t *result = out;
    const uint64_t *value = values;
    for (size_t i = 0; i < count; ++i) {
        result[i] = value[i] + 2;
    }
    return 0;
}

static int keep_even_batch(void *ctx, void *out, size_t *kept, const void *values, size_t count)
{
    uint64_t *result = out;
    const uint64_t *value = values;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        result[n] = value[i];
        n += !(value[i] & 1);
    }
    *kept = n;
    return 0;
}

static int sum_values(void *ctx, void *out, const void *const *values, size_t count)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        sum += *(const uint64_t *)values[i];
    }
    *(uint64_t *)out = sum;
    return 0;
}

static evio_rx_observable *map_add_two(evio_rx_observable *source, bool batch)
{
    if (batch) {
        return evio_rx_map_batch(source, &evio_rx_uint64_type, add_two_batch,
                                 (evio_rx_capture) {0});
    }
    return evio_rx_map(source, &evio_rx_uint64_type, add_two, (evio_rx_capture) {0});
}

static evio_rx_observable *filter_even(evio_rx_observable *source, bool batch)
{
    if (batch) {
        return evio_rx_filter_batch(source, keep_even_batch, (evio_rx_capture) {0});
    }
    return evio_rx_filter(source, is_even, (evio_rx_capture) {0});
}

static uint64_t *make_values(size_t count)
{
    uint64_t *values = evio_malloc(count * sizeof(*values));
    for (size_t i = 0; i < count; ++i) {
        values[i] = i;
    }
    return values;
}

static void run_subscription(evio_loop *loop, evio_rx_observable *source, sink *s,
                             uint64_t *time_ns)
{
    evio_rx_subscription *sub = evio_rx_subscription_new(loop, source, sink_observer(s), 16);
    uint64_t start = get_time_ns();
    evio_rx_subscription_start(sub);
    *time_ns = get_time_ns() - start;
    evio_rx_subscription_free(sub);
}

// --- array -> map ---
static void bench_map(evio_loop *loop)
{
    uint64_t *values = make_values(NUM_VALUES);

    sink native = {0};
    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_VALUES; ++i) {
        uint64_t value = values[i] + 2;
        sink_next(&native, &value);
    }
    sink_complete(&native);
    print_benchmark("rx_map", "native", get_time_ns() - start, NUM_VALUES);

    evio_rx_observable *array = evio_rx_from_array(&evio_rx_uint64_type, values, NUM_VALUES);
    for (int batch = 0; batch < 2; ++batch) {
        evio_rx_observable *map = map_add_two(array, batch);
        sink rx = {0};
        uint64_t time_ns;
        run_subscription(loop, map, &rx, &time_ns);
        check(&rx, native.count, native.sum);
        print_benchmark("rx_map", batch ? "rx_batch" : "rx", time_ns, NUM_VALUES);
        evio_rx_observable_free(map);
    }

    evio_rx_observable_free(array);
    evio_free(values);
}

// --- array -> map -> filter -> map -> filter ---
static void bench_chain(evio_loop *loop)
{
    uint64_t *values = make_values(NUM_VALUES);

    sink native = {0};
    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_VALUES; ++i) {
        uint64_t value = values[i] + 2;
        if (value & 1) {
            continue;
        }
        value += 2;
        if (!(value & 1)) {
            sink_next(&native, &value);
        }
    }
    sink_complete(&native);
    print_benchmark("rx_chain4", "native", get_time_ns() - start, NUM_VALUES);

    for (int batch = 0; batch < 2; ++batch) {
        evio_rx_observable *source = evio_rx_from_array(&evio_rx_uint64_type, values, NUM_VALUES);
        for (size_t i = 0; i < 2; ++i) {
            evio_rx_observable *map = map_add_two(source, batch);
            evio_rx_observable_free(source);
            source = filter_even(map, batch);
            evio_rx_observable_free(map);
        }
        sink rx = {0};
        uint64_t time_ns;
        run_subscription(loop, source, &rx, &time_ns);
        check(&rx, native.count, native.sum);
        print_benchmark("rx_chain4", batch ? "rx_batch" : "rx", time_ns, NUM_VALUES);
        evio_rx_observable_free(source);
    }

    evio_free(values);
}

static const evio_rx_type bool_type = {
    .size = sizeof(bool), .alignment = alignof(bool),
};

static int above_threshold(void *ctx, void *out, const void *value)
{
    *(bool *)out = *(const uint64_t *)value > 100;
    return 0;
}

static bool same_state(void *ctx, const void *previous, const void *value)
{
    return *(const bool *)previous == *(const bool *)value;
}

[[gnu::noinline]]
static void threshold_next(void *ctx, const void *value)
{
    size_t *count = ctx;
    if (*(const bool *)value != (*count & 1)) {
        abort();
    }
    ++*count;
}

static void bench_threshold(evio_loop *loop, const char *name, size_t run_length)
{
    uint64_t *values = evio_malloc(NUM_PUBLISH * sizeof(*values));
    for (size_t i = 0; i < NUM_PUBLISH; ++i) {
        values[i] = (i / run_length) & 1 ? 110 : 80;
    }
    size_t expected = 1 + (NUM_PUBLISH - 1) / run_length;
    size_t count = 0;
    bool previous = false;
    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_PUBLISH; ++i) {
        bool above = values[i] > 100;
        if (!i || above != previous) {
            threshold_next(&count, &above);
            previous = above;
        }
    }
    uint64_t elapsed = get_time_ns() - start;
    if (count != expected) {
        abort();
    }
    print_benchmark(name, "native", elapsed, NUM_PUBLISH);

    count = 0;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &evio_rx_uint64_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *mapped = evio_rx_map(source, &bool_type, above_threshold,
                                             (evio_rx_capture) {0});
    evio_rx_observable *changes = evio_rx_distinct_until_changed(mapped, same_state,
                                                                 (evio_rx_capture) {0});
    evio_rx_observer observer = { .next = threshold_next, .ctx = &count };
    evio_rx_subscription *sub = evio_rx_subscription_new(loop, changes, observer, 1);
    evio_rx_subscription_start(sub);
    start = get_time_ns();
    for (size_t i = 0; i < NUM_PUBLISH; ++i) {
        if (evio_rx_subject_next(subject, &values[i])) {
            abort();
        }
    }
    elapsed = get_time_ns() - start;
    if (count != expected || evio_rx_subscription_is_closed(sub)) {
        abort();
    }
    print_benchmark(name, "rx", elapsed, NUM_PUBLISH);
    evio_rx_subscription_free(sub);
    evio_rx_observable_free(changes);
    evio_rx_observable_free(mapped);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    evio_free(values);
}

// --- subject fan-out to 8 subscribers ---
static void bench_subject(evio_loop *loop)
{
    sink native[NUM_SUBSCRIBERS] = {0};
    void (*callbacks[NUM_SUBSCRIBERS])(void *, const void *);
    for (size_t i = 0; i < NUM_SUBSCRIBERS; ++i) {
        callbacks[i] = sink_next;
    }
    uint64_t start = get_time_ns();
    for (uint64_t value = 0; value < NUM_PUBLISH; ++value) {
        for (size_t i = 0; i < NUM_SUBSCRIBERS; ++i) {
            callbacks[i](&native[i], &value);
        }
    }
    print_benchmark("rx_subject8", "native", get_time_ns() - start, NUM_PUBLISH);

    evio_rx_subject *subject = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    sink rx[NUM_SUBSCRIBERS] = {0};
    evio_rx_subscription *subs[NUM_SUBSCRIBERS];
    for (size_t i = 0; i < NUM_SUBSCRIBERS; ++i) {
        subs[i] = evio_rx_subscription_new(loop, source, sink_observer(&rx[i]), 16);
        evio_rx_subscription_start(subs[i]);
    }
    start = get_time_ns();
    for (uint64_t value = 0; value < NUM_PUBLISH; ++value) {
        if (evio_rx_subject_next(subject, &value)) {
            abort();
        }
    }
    uint64_t end = get_time_ns();
    for (size_t i = 0; i < NUM_SUBSCRIBERS; ++i) {
        check(&rx[i], native[i].count, native[i].sum);
        evio_rx_subscription_free(subs[i]);
    }
    print_benchmark("rx_subject8", "rx", end - start, NUM_PUBLISH);

    uint64_t *values = make_values(NUM_PUBLISH);
    for (size_t i = 0; i < NUM_SUBSCRIBERS; ++i) {
        rx[i] = (sink) {0};
        subs[i] = evio_rx_subscription_new(loop, source, sink_observer(&rx[i]), 16);
        evio_rx_subscription_start(subs[i]);
    }
    start = get_time_ns();
    for (size_t i = 0; i < NUM_PUBLISH; i += 64) {
        size_t count = NUM_PUBLISH - i < 64 ? NUM_PUBLISH - i : 64;
        if (evio_rx_subject_next_batch(subject, values + i, count)) {
            abort();
        }
    }
    end = get_time_ns();
    for (size_t i = 0; i < NUM_SUBSCRIBERS; ++i) {
        check(&rx[i], native[i].count, native[i].sum);
        evio_rx_subscription_free(subs[i]);
    }
    print_benchmark("rx_subject8", "rx_batch", end - start, NUM_PUBLISH);

    evio_free(values);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
}

// --- merge / zip of two subjects ---
typedef struct {
    uint64_t values[2][2];
    size_t heads[2];
    size_t counts[2];
} native_zip;

static void native_zip_push(native_zip *zip, size_t input, uint64_t value, sink *s)
{
    zip->values[input][(zip->heads[input] + zip->counts[input]++) & 1] = value;
    if (!zip->counts[0] || !zip->counts[1]) {
        return;
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < 2; ++i) {
        sum += zip->values[i][zip->heads[i]];
        zip->heads[i] ^= 1;
        zip->counts[i]--;
    }
    sink_next(s, &sum);
}

static void bench_fan_in(evio_loop *loop, bool zip)
{
    const char *name = zip ? "rx_zip2" : "rx_merge2";

    sink native = {0};
    native_zip pairs = {0};
    uint64_t start = get_time_ns();
    for (uint64_t value = 0; value < NUM_PUBLISH; ++value) {
        if (zip) {
            native_zip_push(&pairs, 0, value, &native);
            native_zip_push(&pairs, 1, value, &native);
        } else {
            sink_next(&native, &value);
            sink_next(&native, &value);
        }
    }
    print_benchmark(name, "native", get_time_ns() - start, NUM_PUBLISH * 2);

    evio_rx_subject *subjects[2];
    evio_rx_observable *inputs[2];
    for (size_t i = 0; i < 2; ++i) {
        subjects[i] = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
        inputs[i] = evio_rx_from_subject(subjects[i]);
    }
    evio_rx_combiner combiner = { .type = &evio_rx_uint64_type, .cb = sum_values };
    evio_rx_observable *source = zip ? evio_rx_zip(inputs, 2, combiner, 2) : evio_rx_merge(inputs, 2);
    sink rx = {0};
    evio_rx_subscription *sub = evio_rx_subscription_new(loop, source, sink_observer(&rx), 16);
    evio_rx_subscription_start(sub);
    start = get_time_ns();
    for (uint64_t value = 0; value < NUM_PUBLISH; ++value) {
        if (evio_rx_subject_next(subjects[0], &value) || evio_rx_subject_next(subjects[1], &value)) {
            abort();
        }
    }
    uint64_t end = get_time_ns();
    check(&rx, native.count, native.sum);
    print_benchmark(name, "rx", end - start, NUM_PUBLISH * 2);

    evio_rx_subscription_free(sub);
    evio_rx_observable_free(source);
    for (size_t i = 0; i < 2; ++i) {
        evio_rx_observable_free(inputs[i]);
        evio_rx_subject_free(subjects[i]);
    }
}

// --- subscription lifecycle: new, start, complete, free ---
typedef struct {
    sink *s;
    uint64_t value;
} native_run;

static void bench_lifecycle(evio_loop *loop)
{
    uint64_t one = 1;

    sink native = {0};
    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_CYCLES; ++i) {
        native_run *run = evio_malloc(sizeof(*run));
        run->s = &native;
        run->value = one + 2;
        sink_next(run->s, &run->value);
        sink_complete(run->s);
        evio_free(run);
    }
    print_benchmark("rx_lifecycle", "native", get_time_ns() - start, NUM_CYCLES);

    sink native_reused = {0};
    native_run *run = evio_malloc(sizeof(*run));
    start = get_time_ns();
    for (size_t i = 0; i < NUM_CYCLES; ++i) {
        run->s = &native_reused;
        run->value = one + 2;
        sink_next(run->s, &run->value);
        sink_complete(run->s);
    }
    print_benchmark("rx_lifecycle", "native_reuse", get_time_ns() - start, NUM_CYCLES);
    evio_free(run);
    check(&native_reused, native.count, native.sum);

    evio_rx_observable *array = evio_rx_from_array(&evio_rx_uint64_type, &one, 1);
    evio_rx_observable *map = evio_rx_map(array, &evio_rx_uint64_type, add_two,
                                          (evio_rx_capture) {0});
    sink rx = {0};
    start = get_time_ns();
    for (size_t i = 0; i < NUM_CYCLES; ++i) {
        evio_rx_subscription *sub = evio_rx_subscription_new(loop, map, sink_observer(&rx), 16);
        evio_rx_subscription_start(sub);
        evio_rx_subscription_free(sub);
    }
    uint64_t end = get_time_ns();
    check(&rx, native.count, native.sum);
    print_benchmark("rx_lifecycle", "rx", end - start, NUM_CYCLES);

    sink reused = {0};
    evio_rx_subscription *sub = evio_rx_subscription_new(loop, map, sink_observer(&reused), 16);
    start = get_time_ns();
    for (size_t i = 0; i < NUM_CYCLES; ++i) {
        evio_rx_subscription_restart(sub);
    }
    end = get_time_ns();
    evio_rx_subscription_free(sub);
    check(&reused, native.count, native.sum);
    print_benchmark("rx_lifecycle", "rx_restart", end - start, NUM_CYCLES);

    evio_rx_observable_free(map);
    evio_rx_observable_free(array);
}

// --- subject with 8 subscribers: subscribe, complete, free ---
typedef struct native_listener {
    struct native_listener *next;
    void (*complete)(void *ctx);
    void *ctx;
} native_listener;

static void bench_subject_close(evio_loop *loop)
{
    sink native = {0};
    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_CYCLES / NUM_SUBSCRIBERS; ++i) {
        native_listener **first = evio_malloc(sizeof(*first));
        *first = NULL;
        for (size_t j = 0; j < NUM_SUBSCRIBERS; ++j) {
            native_listener *listener = evio_malloc(sizeof(*listener));
            *listener = (native_listener) { .next = *first, .complete = sink_complete, .ctx = &native };
            *first = listener;
        }
        while (*first) {
            native_listener *listener = *first;
            *first = listener->next;
            listener->complete(listener->ctx);
            evio_free(listener);
        }
        evio_free(first);
    }
    print_benchmark("rx_subject_close", "native", get_time_ns() - start, NUM_CYCLES);

    sink rx = {0};
    start = get_time_ns();
    for (size_t i = 0; i < NUM_CYCLES / NUM_SUBSCRIBERS; ++i) {
        evio_rx_subject *subject = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_subscription *subs[NUM_SUBSCRIBERS];
        for (size_t j = 0; j < NUM_SUBSCRIBERS; ++j) {
            subs[j] = evio_rx_subscription_new(loop, source, sink_observer(&rx), 16);
            evio_rx_subscription_start(subs[j]);
        }
        evio_rx_subject_complete(subject);
        for (size_t j = 0; j < NUM_SUBSCRIBERS; ++j) {
            evio_rx_subscription_free(subs[j]);
        }
        evio_rx_observable_free(source);
        evio_rx_subject_free(subject);
    }
    uint64_t end = get_time_ns();
    check(&rx, native.count, native.sum);
    print_benchmark("rx_subject_close", "rx", end - start, NUM_CYCLES);

    sink reused = {0};
    evio_rx_subject *subject = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_subscription *subs[NUM_SUBSCRIBERS];
    for (size_t j = 0; j < NUM_SUBSCRIBERS; ++j) {
        subs[j] = evio_rx_subscription_new(loop, source, sink_observer(&reused), 16);
    }
    start = get_time_ns();
    for (size_t i = 0; i < NUM_CYCLES / NUM_SUBSCRIBERS; ++i) {
        evio_rx_subject_reopen(subject);
        for (size_t j = 0; j < NUM_SUBSCRIBERS; ++j) {
            evio_rx_subscription_restart(subs[j]);
        }
        evio_rx_subject_complete(subject);
    }
    end = get_time_ns();
    for (size_t j = 0; j < NUM_SUBSCRIBERS; ++j) {
        evio_rx_subscription_free(subs[j]);
    }
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    check(&reused, native.count, native.sum);
    print_benchmark("rx_subject_close", "rx_reopen", end - start, NUM_CYCLES);
}

// --- one-shot timers fired by the loop ---
static void native_timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    uint64_t value = 0;
    sink_next(base->data, &value);
    sink_complete(base->data);
}

static void native_timer_once_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    native_timer_cb(loop, base, emask);
    evio_free(base);
}

static void bench_timer(evio_loop *loop)
{
    sink native = {0};
    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_TIMERS; ++i) {
        evio_timer *timer = evio_malloc(sizeof(*timer));
        evio_timer_init(timer, native_timer_once_cb, 0);
        timer->data = &native;
        evio_timer_start(loop, timer, 0);
    }
    evio_run(loop, EVIO_RUN_DEFAULT);
    print_benchmark("rx_timer", "native", get_time_ns() - start, NUM_TIMERS);

    evio_rx_observable *timer = evio_rx_timer(0);
    evio_rx_subscription **subs = evio_calloc(NUM_TIMERS, sizeof(*subs));
    sink rx = {0};
    start = get_time_ns();
    for (size_t i = 0; i < NUM_TIMERS; ++i) {
        subs[i] = evio_rx_subscription_new(loop, timer, sink_observer(&rx), 16);
        evio_rx_subscription_start(subs[i]);
    }
    evio_run(loop, EVIO_RUN_DEFAULT);
    for (size_t i = 0; i < NUM_TIMERS; ++i) {
        evio_rx_subscription_free(subs[i]);
    }
    uint64_t end = get_time_ns();
    check(&rx, native.count, native.sum);
    print_benchmark("rx_timer", "rx", end - start, NUM_TIMERS);

    evio_free(subs);
    evio_rx_observable_free(timer);
}

// --- debounce: every value rearms the timer ---
static void bench_debounce(evio_loop *loop)
{
    evio_timer timer;
    evio_timer_init(&timer, native_timer_cb, 0);
    uint64_t last = 0;
    uint64_t start = get_time_ns();
    for (uint64_t value = 0; value < NUM_PUBLISH; ++value) {
        last = value;
        evio_timer_stop(loop, &timer);
        evio_timer_start(loop, &timer, EVIO_TIME_FROM_SEC(60));
    }
    evio_timer_stop(loop, &timer);
    print_benchmark("rx_debounce", "native", get_time_ns() - start, NUM_PUBLISH);

    evio_rx_subject *subject = evio_rx_subject_new(loop, &evio_rx_uint64_type, 16);
    evio_rx_observable *input = evio_rx_from_subject(subject);
    evio_rx_observable *source = evio_rx_debounce_time(input, EVIO_TIME_FROM_SEC(60));
    sink rx = {0};
    evio_rx_subscription *sub = evio_rx_subscription_new(loop, source, sink_observer(&rx), 16);
    evio_rx_subscription_start(sub);
    start = get_time_ns();
    for (uint64_t value = 0; value < NUM_PUBLISH; ++value) {
        if (evio_rx_subject_next(subject, &value)) {
            abort();
        }
    }
    evio_rx_subject_complete(subject);
    uint64_t end = get_time_ns();
    check(&rx, 2, last);
    print_benchmark("rx_debounce", "rx", end - start, NUM_PUBLISH);

    evio_rx_subscription_free(sub);
    evio_rx_observable_free(source);
    evio_rx_observable_free(input);
    evio_rx_subject_free(subject);
}

// --- switch_map: every outer value replaces the inner run ---
static evio_rx_observable *project_inner(void *ctx, const void *value)
{
    evio_rx_observable **inner = ctx;
    return evio_rx_observable_ref(inner[*(const uint64_t *)value & 1]);
}

// The inner stays open until the next outer value replaces it.
static int hold_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    *state = ctx;
    return 0;
}

static void hold_start(evio_rx_emitter *emitter, void *state)
{
    if (!evio_rx_emitter_next(emitter, state)) {
        return;
    }
}

static evio_rx_observable *project_hold(void *ctx, const void *value)
{
    return evio_rx_observable_ref(ctx);
}

static void bench_switch(evio_loop *loop)
{
    uint64_t *values = make_values(NUM_CYCLES);

    sink native = {0};
    uint64_t start = get_time_ns();
    for (size_t i = 0; i < NUM_CYCLES; ++i) {
        native_run *run = evio_malloc(sizeof(*run));
        run->s = &native;
        run->value = values[i] & 1;
        sink_next(run->s, &run->value);
        evio_free(run);
    }
    sink_complete(&native);
    print_benchmark("rx_switch", "native", get_time_ns() - start, NUM_CYCLES);

    evio_rx_observable *outer = evio_rx_from_array(&evio_rx_uint64_type, values, NUM_CYCLES);
    evio_rx_observable *inner[] = {
        evio_rx_from_array(&evio_rx_uint64_type, &values[0], 1),
        evio_rx_from_array(&evio_rx_uint64_type, &values[1], 1),
    };
    evio_rx_project project = {
        .type = &evio_rx_uint64_type,
        .cb = project_inner,
        .capture = { .data = inner },
    };
    evio_rx_observable *source = evio_rx_switch_map(outer, project);
    sink rx = {0};
    uint64_t time_ns;
    run_subscription(loop, source, &rx, &time_ns);
    check(&rx, native.count, native.sum);
    print_benchmark("rx_switch", "rx", time_ns, NUM_CYCLES);
    evio_rx_observable_free(source);

    uint64_t one = 1;
    evio_rx_source hold_source = { .prepare = hold_prepare, .start = hold_start };
    evio_rx_observable *hold = evio_rx_observable_new(&evio_rx_uint64_type, hold_source,
                                                      (evio_rx_capture) { .data = &one });
    project.cb = project_hold;
    project.capture.data = hold;
    source = evio_rx_switch_map(outer, project);
    sink held = {0};
    run_subscription(loop, source, &held, &time_ns);
    check(&held, NUM_CYCLES, NUM_CYCLES);
    print_benchmark("rx_switch", "rx_same_inner", time_ns, NUM_CYCLES);

    evio_rx_observable_free(source);
    evio_rx_observable_free(hold);
    evio_rx_observable_free(inner[0]);
    evio_rx_observable_free(inner[1]);
    evio_rx_observable_free(outer);
    evio_free(values);
}

int main(void)
{
    print_versions();

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);

    bench_map(loop);
    bench_chain(loop);
    bench_threshold(loop, "rx_threshold_sparse", 64);
    bench_threshold(loop, "rx_threshold_alternating", 1);
    bench_subject(loop);
    bench_fan_in(loop, false);
    bench_fan_in(loop, true);
    bench_lifecycle(loop);
    bench_subject_close(loop);
    bench_timer(loop);
    bench_debounce(loop);
    bench_switch(loop);

    evio_loop_free(loop);
    return EXIT_SUCCESS;
}
