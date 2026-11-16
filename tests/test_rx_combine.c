#include "test.h"
#include "evio_rx.h"

static const evio_rx_type int_type = {
    .size = sizeof(int), .alignment = alignof(int),
};

typedef struct combine_log combine_log;
struct combine_log {
    evio_loop *loop;
    evio_rx_subscription *subscription;
    evio_rx_subject *other;
    int values[16];
    size_t count;
    size_t completed;
    int error;
    size_t depth;
    bool cancel;
    bool overflow;
    void (*action)(combine_log *log);
};

static void record_next(void *ctx, const void *value)
{
    combine_log *log = ctx;
    assert_int_equal(log->depth++, 0);
    assert_true(log->count < 16);
    log->values[log->count++] = *(const int *)value;
    if (log->action) {
        log->action(log);
    }
    if (log->cancel) {
        evio_rx_subscription_cancel(log->subscription);
    }
    log->depth--;
}

static void record_error(void *ctx, int error)
{
    combine_log *log = ctx;
    assert_int_equal(log->depth, 0);
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->error = error;
}

static void record_complete(void *ctx)
{
    combine_log *log = ctx;
    assert_int_equal(log->depth, 0);
    assert_int_equal(log->completed, 0);
    assert_int_equal(log->error, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->completed++;
}

static evio_rx_observer observer(combine_log *log)
{
    return (evio_rx_observer) {
        .next = record_next, .error = record_error, .complete = record_complete, .ctx = log,
    };
}

TEST(test_evio_rx_merge_synchronous_inputs_and_independent_runs)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int a[] = {1, 2};
    int b[] = {10, 20};
    for (size_t count = 1; count <= 2; ++count) {
        evio_rx_observable *sources[] = {
            evio_rx_from_array(&int_type, a, 2), evio_rx_from_array(&int_type, b, 2),
        };
        evio_rx_observable *merged = evio_rx_merge(sources, count);
        assert_non_null(merged);
        evio_rx_observable_free(sources[0]);
        evio_rx_observable_free(sources[1]);
        combine_log logs[2] = {0};
        for (size_t i = 0; i < 2; ++i) {
            logs[i].subscription = evio_rx_subscription_new(loop, merged, observer(&logs[i]), 1);
            assert_non_null(logs[i].subscription);
        }
        evio_rx_observable_free(merged);
        for (size_t i = 0; i < 2; ++i) {
            evio_rx_subscription_start(logs[i].subscription);
            int expected[] = {1, 2, 10, 20};
            assert_int_equal(logs[i].count, count * 2);
            assert_memory_equal(logs[i].values, expected, count * 2 * sizeof(int));
            assert_int_equal(logs[i].completed, 1);
            assert_int_equal(logs[i].error, 0);
            evio_rx_subscription_free(logs[i].subscription);
        }
    }
    assert_int_equal(loop->refcount, 0);
    evio_loop_free(loop);
}

static void publish_other(combine_log *log)
{
    if (log->count != 1) {
        return;
    }
    int value = 2;
    assert_int_equal(evio_rx_subject_next(log->other, &value), 0);
    value = 99;
    if (log->overflow) {
        value = 3;
        assert_int_equal(evio_rx_subject_next(log->other, &value), 0);
    }
    assert_int_equal(log->count, 1);
    assert_int_equal(log->error, 0);
}

TEST(test_evio_rx_merge_cross_subject_reentry_is_owned_and_serialized)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *a = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_subject *b = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_observable *sources[] = {evio_rx_from_subject(a), evio_rx_from_subject(b)};
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    combine_log log = { .other = b, .action = publish_other };
    log.subscription = evio_rx_subscription_new(loop, merged, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    int expected[] = {1, 2};
    assert_int_equal(log.count, 2);
    assert_memory_equal(log.values, expected, sizeof(expected));
    evio_rx_subject_complete(a);
    assert_int_equal(log.completed, 0);
    evio_rx_subject_complete(b);
    assert_int_equal(log.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(a);
    evio_rx_subject_free(b);
    evio_loop_free(loop);
}

static void publish_other_wrapping(combine_log *log)
{
    int values[] = {2, 3, 4};
    if (log->count == 1) {
        assert_int_equal(evio_rx_subject_next_batch(log->other, values, 2), 0);
    } else if (log->count == 2) {
        assert_int_equal(evio_rx_subject_next(log->other, &values[2]), 0);
    }
}

TEST(test_evio_rx_merge_reentry_wraps_root_fifo)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *a = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_subject *b = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *sources[] = {evio_rx_from_subject(a), evio_rx_from_subject(b)};
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    combine_log log = { .other = b, .action = publish_other_wrapping };
    log.subscription = evio_rx_subscription_new(loop, merged, observer(&log), 2);
    evio_rx_subscription_start(log.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    int expected[] = {1, 2, 3, 4};
    assert_int_equal(log.count, 4);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(log.error, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(a);
    evio_rx_subject_free(b);
    evio_loop_free(loop);
}

TEST(test_evio_rx_merge_root_overflow_does_not_close_subjects)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *a = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_subject *b = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_observable *sources[] = {evio_rx_from_subject(a), evio_rx_from_subject(b)};
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    combine_log log = { .other = b, .action = publish_other, .overflow = true };
    combine_log other = {0};
    log.subscription = evio_rx_subscription_new(loop, merged, observer(&log), 1);
    other.subscription = evio_rx_subscription_new(loop, sources[1], observer(&other), 1);
    evio_rx_subscription_start(log.subscription);
    evio_rx_subscription_start(other.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.error, EOVERFLOW);
    assert_int_equal(other.count, 2);
    assert_int_equal(other.values[0], 2);
    assert_int_equal(other.values[1], 3);
    assert_int_equal(other.error, 0);
    value = 4;
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    assert_int_equal(evio_rx_subject_next(b, &value), 0);
    assert_int_equal(other.count, 3);
    assert_int_equal(other.values[2], 4);
    evio_rx_subject_complete(a);
    evio_rx_subject_complete(b);
    assert_int_equal(log.completed, 0);
    assert_int_equal(other.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_subscription_free(other.subscription);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(a);
    evio_rx_subject_free(b);
    evio_loop_free(loop);
}

typedef struct {
    evio_rx_subject *other;
    size_t prepared;
    size_t started;
    size_t stopped;
    size_t destroyed;
    int error;
    int prepare_error;
} start_log;

typedef struct {
    start_log *log;
} start_state;

static int start_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    start_log *log = ctx;
    if (log->prepare_error) {
        return log->prepare_error;
    }
    start_state *producer = evio_calloc(1, sizeof(*producer));
    producer->log = log;
    log->prepared++;
    *state = producer;
    return 0;
}

static void start_producer(evio_rx_emitter *emitter, void *state)
{
    start_state *producer = state;
    start_log *log = producer->log;
    log->started++;
    if (log->error) {
        evio_rx_emitter_error(emitter, log->error);
        return;
    }
    if (log->other) {
        int b = 2;
        assert_int_equal(evio_rx_subject_next(log->other, &b), 0);
    }
    int a = 1;
    if (evio_rx_emitter_next(emitter, &a)) {
        evio_rx_emitter_complete(emitter);
    }
}

static void stop_producer(void *state)
{
    start_state *producer = state;
    producer->log->stopped++;
}

static void destroy_producer(void *state)
{
    start_state *producer = state;
    producer->log->destroyed++;
    evio_free(producer);
}

static const evio_rx_source start_source = {
    .prepare = start_prepare, .start = start_producer,
    .stop = stop_producer, .destroy = destroy_producer,
};

TEST(test_evio_rx_merge_synchronous_error_prevents_later_start)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    start_log first = { .error = EIO };
    start_log second = {0};
    evio_rx_observable *sources[] = {
        evio_rx_observable_new(&int_type, start_source, (evio_rx_capture) { .data = &first }),
        evio_rx_observable_new(&int_type, start_source, (evio_rx_capture) { .data = &second }),
    };
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    combine_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, merged, observer(&log), 1);
    assert_non_null(log.subscription);
    assert_int_equal(first.prepared, 1);
    assert_int_equal(second.prepared, 1);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.error, EIO);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(first.started, 1);
    assert_int_equal(second.started, 0);
    assert_int_equal(first.stopped, 1);
    assert_int_equal(second.stopped, 0);
    assert_int_equal(first.destroyed, 1);
    assert_int_equal(second.destroyed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_loop_free(loop);
}

TEST(test_evio_rx_merge_foreign_producer_does_not_overtake_backlog)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    start_log producer = { .other = subject };
    evio_rx_observable *sources[] = {
        evio_rx_from_subject(subject),
        evio_rx_observable_new(&int_type, start_source, (evio_rx_capture) { .data = &producer }),
    };
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    combine_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, merged, observer(&log), 1);
    assert_non_null(log.subscription);
    evio_rx_subscription_start(log.subscription);
    int expected[] = {2, 1};
    assert_int_equal(log.count, 2);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(log.error, 0);
    assert_int_equal(log.completed, 0);
    evio_rx_subject_complete(subject);
    assert_int_equal(log.completed, 1);
    assert_int_equal(producer.stopped, 1);
    assert_int_equal(producer.destroyed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

static int uint64_to_int(void *ctx, void *out, const void *value)
{
    *(int *)out = (int) * (const uint64_t *)value;
    return 0;
}

static void nested_pending(combine_log *log)
{
    if (log->count == 1) {
        evio_invoke_pending(log->loop);
        assert_int_equal(log->count, 1);
        assert_int_equal(log->error, 0);
    }
}

TEST(test_evio_rx_merge_nested_native_pending_is_serialized)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_observable *timer = evio_rx_timer(0);
    evio_rx_observable *sources[] = {
        evio_rx_from_subject(subject),
        evio_rx_map(timer, &int_type, uint64_to_int, (evio_rx_capture) {0}),
    };
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    combine_log log = { .loop = loop, .action = nested_pending };
    log.subscription = evio_rx_subscription_new(loop, merged, observer(&log), 2);
    assert_non_null(log.subscription);
    evio_rx_subscription_start(log.subscription);
    evio_timer_update(loop);
    assert_true(evio_pending_count(loop) > 0);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    int expected[] = {1, 0};
    assert_int_equal(log.count, 2);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(log.error, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subject_complete(subject);
    assert_int_equal(log.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(timer);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_merge_cancel_stops_all_watchers)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_observable *sources[] = {evio_rx_timer(0), evio_rx_timer(0)};
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    evio_rx_subscription *subscription = evio_rx_subscription_new(loop, merged, (evio_rx_observer) {0}, 2);
    evio_rx_subscription_start(subscription);
    assert_int_equal(loop->refcount, 2);
    evio_timer_update(loop);
    evio_rx_subscription_cancel(subscription);
    evio_invoke_pending(loop);
    assert_int_equal(loop->refcount, 0);
    assert_int_equal(evio_pending_count(loop), 0);
    evio_rx_subscription_free(subscription);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_loop_free(loop);
}

typedef struct {
    alignas(64) const void *self;
    int *number;
} owned_number;

typedef struct {
    combine_log log;
    size_t copy_calls;
    size_t created;
    size_t destroyed;
    size_t live;
    size_t combine_calls;
    size_t fail_copy;
    size_t capture_destroyed;
    bool cancel_in_combine;
    bool cancel_in_copy;
} owned_log;

static int owned_copy(void *ctx, void *dst, const void *src)
{
    owned_log *log = ctx;
    const owned_number *value = src;
    owned_number *out = dst;
    assert_ptr_equal(value->self, value);
    assert_int_equal((uintptr_t)out % alignof(owned_number), 0);
    if (++log->copy_calls == log->fail_copy) {
        return ENOSPC;
    }
    out->self = out;
    out->number = evio_malloc(sizeof(*out->number));
    *out->number = *value->number;
    log->created++;
    log->live++;
    if (log->cancel_in_copy) {
        evio_rx_subscription_cancel(log->log.subscription);
        assert_ptr_equal(out->self, out);
        assert_int_equal(*out->number, *value->number);
    }
    return 0;
}

static void owned_destroy(void *ctx, void *value)
{
    owned_log *log = ctx;
    owned_number *number = value;
    assert_ptr_equal(number->self, number);
    assert_true(log->live > 0);
    evio_free(number->number);
    log->live--;
    log->destroyed++;
}

static evio_rx_type owned_type(owned_log *log)
{
    return (evio_rx_type) {
        .size = sizeof(owned_number), .alignment = alignof(owned_number),
        .copy = owned_copy, .destroy = owned_destroy, .ctx = log,
    };
}

static int owned_sum(void *ctx, void *out, const void *const *values, size_t count)
{
    owned_log *log = ctx;
    assert_int_equal(count, 2);
    const owned_number *a = values[0];
    assert_ptr_equal(a->self, a);
    log->combine_calls++;
    if (log->cancel_in_combine) {
        evio_rx_subscription_cancel(log->log.subscription);
        assert_ptr_equal(a->self, a);
        assert_true(log->live > 0);
    }
    *(int *)out = *a->number + *(const int *)values[1];
    return 0;
}

static int sum_values(void *ctx, void *out, const void *const *values, size_t count)
{
    int sum = 0;
    for (size_t i = 0; i < count; ++i) {
        sum += *(const int *)values[i];
    }
    *(int *)out = sum;
    return 0;
}

TEST(test_evio_rx_fan_in_input_prepare_failure_fails_new)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    start_log failing = { .prepare_error = EACCES };
    int value = 1;
    evio_rx_observable *inputs[] = {
        evio_rx_from_array(&int_type, &value, 1),
        evio_rx_observable_new(&int_type, start_source, (evio_rx_capture) { .data = &failing }),
    };
    evio_rx_combiner combiner = { .type = &int_type, .cb = sum_values };
    evio_rx_observable *operators[] = {
        evio_rx_merge(inputs, 2),
        evio_rx_combine_latest(inputs, 2, combiner),
        evio_rx_zip(inputs, 2, combiner, 1),
        evio_rx_take_until(inputs[0], inputs[1]),
    };
    for (size_t i = 0; i < 4; ++i) {
        combine_log log = {0};
        assert_null(evio_rx_subscription_new(loop, operators[i], observer(&log), 1));
        assert_int_equal(errno, EACCES);
        assert_int_equal(loop->refcount, 0);
        evio_rx_observable_free(operators[i]);
    }
    evio_rx_observable_free(inputs[0]);
    evio_rx_observable_free(inputs[1]);
    evio_loop_free(loop);
}

TEST(test_evio_rx_combine_latest_mixed_owned_inputs_and_completed_latest)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    owned_log data = {0};
    evio_rx_type type = owned_type(&data);
    evio_rx_subject *a = evio_rx_subject_new(loop, &type, 1);
    evio_rx_subject *b = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *sources[] = {evio_rx_from_subject(a), evio_rx_from_subject(b)};
    evio_rx_combiner combiner = {
        .type = &int_type,
        .cb = owned_sum,
        .capture = { .data = &data },
    };
    evio_rx_observable *combined = evio_rx_combine_latest(sources, 2, combiner);
    assert_non_null(combined);
    data.log.subscription = evio_rx_subscription_new(loop, combined, observer(&data.log), 1);
    evio_rx_subscription_start(data.log.subscription);
    int number = 1;
    owned_number value = { .self = &value, .number = &number };
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    number = 2;
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    number = 99;
    assert_int_equal(data.log.count, 0);
    int other = 10;
    assert_int_equal(evio_rx_subject_next(b, &other), 0);
    number = 3;
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    number = 99;
    evio_rx_subject_complete(a);
    assert_int_equal(data.log.completed, 0);
    assert_int_equal(data.live, 1);
    other = 20;
    assert_int_equal(evio_rx_subject_next(b, &other), 0);
    int expected[] = {12, 13, 23};
    assert_int_equal(data.log.count, 3);
    assert_memory_equal(data.log.values, expected, sizeof(expected));
    assert_int_equal(data.combine_calls, 3);
    evio_rx_subject_complete(b);
    assert_int_equal(data.log.completed, 1);
    assert_int_equal(data.log.error, 0);
    assert_int_equal(data.created, 3);
    assert_int_equal(data.destroyed, 3);
    assert_int_equal(data.live, 0);
    evio_rx_subscription_free(data.log.subscription);
    evio_rx_observable_free(combined);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(a);
    evio_rx_subject_free(b);
    evio_loop_free(loop);
}

TEST(test_evio_rx_combine_latest_local_completion_releases_owned_latest)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    owned_log data = {0};
    evio_rx_type type = owned_type(&data);
    int number = 1;
    owned_number value = { .self = &value, .number = &number };
    evio_rx_observable *inputs[] = {
        evio_rx_from_array(&type, &value, 1), evio_rx_from_array(&int_type, NULL, 0),
    };
    evio_rx_combiner combiner = {
        .type = &int_type,
        .cb = owned_sum,
        .capture = { .data = &data },
    };
    evio_rx_observable *timer = evio_rx_timer(20);
    evio_rx_observable *sources[] = {
        evio_rx_combine_latest(inputs, 2, combiner),
        evio_rx_map(timer, &int_type, uint64_to_int, (evio_rx_capture) {0}),
    };
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    data.log.subscription = evio_rx_subscription_new(loop, merged, observer(&data.log), 1);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(timer);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_observable_free(inputs[0]);
    evio_rx_observable_free(inputs[1]);
    evio_rx_subscription_start(data.log.subscription);
    assert_int_equal(data.created, 2);
    assert_int_equal(data.destroyed, 1);
    assert_int_equal(data.live, 1);
    assert_int_equal(data.log.completed, 0);
    assert_int_equal(loop->refcount, 1);
    loop->time = 120;
    evio_timer_update(loop);
    evio_invoke_pending(loop);
    assert_int_equal(data.log.count, 1);
    assert_int_equal(data.log.completed, 1);
    assert_int_equal(data.destroyed, 1);
    assert_int_equal(data.live, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(data.log.subscription);
    assert_int_equal(data.destroyed, 2);
    assert_int_equal(data.live, 0);
    evio_loop_free(loop);
}

TEST(test_evio_rx_combine_latest_cancel_keeps_combiner_inputs_alive)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    owned_log data = { .cancel_in_combine = true };
    evio_rx_type type = owned_type(&data);
    evio_rx_subject *a = evio_rx_subject_new(loop, &type, 1);
    evio_rx_subject *b = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *sources[] = {evio_rx_from_subject(a), evio_rx_from_subject(b)};
    evio_rx_combiner combiner = {
        .type = &int_type,
        .cb = owned_sum,
        .capture = { .data = &data },
    };
    evio_rx_observable *combined = evio_rx_combine_latest(sources, 2, combiner);
    data.log.subscription = evio_rx_subscription_new(loop, combined, observer(&data.log), 1);
    evio_rx_subscription_start(data.log.subscription);
    int number = 1;
    owned_number value = { .self = &value, .number = &number };
    int other = 10;
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    assert_int_equal(evio_rx_subject_next(b, &other), 0);
    assert_int_equal(data.combine_calls, 1);
    assert_int_equal(data.log.count, 0);
    assert_int_equal(data.log.completed, 0);
    assert_int_equal(data.live, 0);
    assert_int_equal(data.created, 1);
    assert_int_equal(data.destroyed, 1);
    evio_rx_subscription_free(data.log.subscription);
    evio_rx_observable_free(combined);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(a);
    evio_rx_subject_free(b);
    evio_loop_free(loop);
}

TEST(test_evio_rx_combine_latest_independent_synchronous_runs)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int a[] = {1, 2};
    int b[] = {10, 20};
    evio_rx_observable *sources[] = {
        evio_rx_from_array(&int_type, a, 2), evio_rx_from_array(&int_type, b, 2),
    };
    evio_rx_combiner combiner = { .type = &int_type, .cb = sum_values };
    evio_rx_observable *combined = evio_rx_combine_latest(sources, 2, combiner);
    combine_log logs[2] = {0};
    for (size_t i = 0; i < 2; ++i) {
        logs[i].subscription = evio_rx_subscription_new(loop, combined, observer(&logs[i]), 1);
    }
    evio_rx_observable_free(combined);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    for (size_t i = 0; i < 2; ++i) {
        evio_rx_subscription_start(logs[i].subscription);
        int expected[] = {12, 22};
        assert_int_equal(logs[i].count, 2);
        assert_memory_equal(logs[i].values, expected, sizeof(expected));
        assert_int_equal(logs[i].completed, 1);
        evio_rx_subscription_free(logs[i].subscription);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_fan_in_restart_repeats_synchronous_run)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int a[] = {1, 2};
    int b[] = {10, 20};
    evio_rx_observable *sources[] = {
        evio_rx_from_array(&int_type, a, 2), evio_rx_from_array(&int_type, b, 2),
    };
    evio_rx_combiner combiner = { .type = &int_type, .cb = sum_values };
    evio_rx_observable *operators[] = {
        evio_rx_merge(sources, 2),
        evio_rx_combine_latest(sources, 2, combiner),
        evio_rx_zip(sources, 2, combiner, 2),
    };
    int expected[][4] = { {1, 2, 10, 20}, {12, 22}, {11, 22} };
    size_t counts[] = {4, 2, 2};
    for (size_t i = 0; i < 3; ++i) {
        combine_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, operators[i], observer(&log), 1);
        assert_non_null(log.subscription);
        for (size_t run = 0; run < 2; ++run) {
            log = (combine_log) { .subscription = log.subscription };
            if (run) {
                evio_rx_subscription_restart(log.subscription);
            } else {
                evio_rx_subscription_start(log.subscription);
            }
            assert_int_equal(log.count, counts[i]);
            assert_memory_equal(log.values, expected[i], counts[i] * sizeof(int));
            assert_int_equal(log.completed, 1);
        }
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(operators[i]);
    }
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_loop_free(loop);
}

TEST(test_evio_rx_combine_latest_empty_input_does_not_start_later_producer)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    start_log producer = {0};
    evio_rx_observable *sources[] = {
        evio_rx_from_array(&int_type, NULL, 0),
        evio_rx_observable_new(&int_type, start_source, (evio_rx_capture) { .data = &producer }),
    };
    evio_rx_combiner combiner = { .type = &int_type, .cb = sum_values };
    evio_rx_observable *combined = evio_rx_combine_latest(sources, 2, combiner);
    combine_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, combined, observer(&log), 1);
    assert_int_equal(producer.prepared, 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(producer.started, 0);
    assert_int_equal(producer.destroyed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(combined);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_loop_free(loop);
}

TEST(test_evio_rx_combine_latest_empty_completion_cancels_only_its_children)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_observable *inputs[] = {evio_rx_timer(10), evio_rx_from_array(&int_type, NULL, 0)};
    evio_rx_combiner combiner = { .type = &int_type, .cb = sum_values };
    evio_rx_observable *timer = evio_rx_timer(20);
    evio_rx_observable *sources[] = {
        evio_rx_combine_latest(inputs, 2, combiner),
        evio_rx_map(timer, &int_type, uint64_to_int, (evio_rx_capture) {0}),
    };
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    combine_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, merged, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(loop->refcount, 1);
    assert_int_equal(log.completed, 0);
    loop->time = 110;
    evio_timer_update(loop);
    assert_int_equal(evio_pending_count(loop), 0);
    loop->time = 120;
    evio_timer_update(loop);
    evio_invoke_pending(loop);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.values[0], 0);
    assert_int_equal(log.completed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(timer);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_observable_free(inputs[0]);
    evio_rx_observable_free(inputs[1]);
    evio_loop_free(loop);
}

typedef struct {
    int error;
    size_t calls;
} combine_failure;

static int fail_combine(void *ctx, void *out, const void *const *values, size_t count)
{
    combine_failure *failure = ctx;
    failure->calls++;
    return failure->error;
}

TEST(test_evio_rx_combine_latest_combiner_error_stops_remaining_values)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int a = 1;
    int b[] = {10, 20};
    int errors[] = {EIO, -1};
    for (size_t i = 0; i < 2; ++i) {
        evio_rx_observable *sources[] = {
            evio_rx_from_array(&int_type, &a, 1), evio_rx_from_array(&int_type, b, 2),
        };
        combine_failure failure = { .error = errors[i] };
        evio_rx_combiner combiner = {
            .type = &int_type,
            .cb = fail_combine,
            .capture = { .data = &failure },
        };
        evio_rx_observable *combined = evio_rx_combine_latest(sources, 2, combiner);
        combine_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, combined, observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, 0);
        assert_int_equal(log.completed, 0);
        assert_int_equal(log.error, i ? EINVAL : EIO);
        assert_int_equal(failure.calls, 1);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(combined);
        evio_rx_observable_free(sources[0]);
        evio_rx_observable_free(sources[1]);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_combine_latest_copy_failure_releases_retained_values)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    owned_log data = { .fail_copy = 2 };
    evio_rx_type type = owned_type(&data);
    evio_rx_subject *a = evio_rx_subject_new(loop, &type, 1);
    evio_rx_subject *b = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *sources[] = {evio_rx_from_subject(a), evio_rx_from_subject(b)};
    evio_rx_combiner combiner = {
        .type = &int_type,
        .cb = owned_sum,
        .capture = { .data = &data },
    };
    evio_rx_observable *combined = evio_rx_combine_latest(sources, 2, combiner);
    data.log.subscription = evio_rx_subscription_new(loop, combined, observer(&data.log), 1);
    evio_rx_subscription_start(data.log.subscription);
    int number = 1;
    owned_number value = { .self = &value, .number = &number };
    int other = 10;
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    assert_int_equal(evio_rx_subject_next(b, &other), 0);
    number = 2;
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    assert_int_equal(data.log.count, 1);
    assert_int_equal(data.log.values[0], 11);
    assert_int_equal(data.log.error, ENOSPC);
    assert_int_equal(data.log.completed, 0);
    assert_int_equal(data.combine_calls, 1);
    assert_int_equal(data.created, 1);
    assert_int_equal(data.destroyed, 1);
    assert_int_equal(data.live, 0);
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    assert_int_equal(evio_rx_subject_next(b, &other), 0);
    assert_int_equal(data.copy_calls, 2);
    evio_rx_subscription_free(data.log.subscription);
    evio_rx_observable_free(combined);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(a);
    evio_rx_subject_free(b);
    evio_loop_free(loop);
}

static int owned_result(void *ctx, void *out, const void *const *values, size_t count)
{
    owned_log *log = ctx;
    owned_number *value = out;
    assert_int_equal((uintptr_t)value % alignof(owned_number), 0);
    value->self = value;
    value->number = evio_malloc(sizeof(*value->number));
    *value->number = 0;
    for (size_t i = 0; i < count; ++i) {
        *value->number += *(const int *)values[i];
    }
    log->combine_calls++;
    log->created++;
    log->live++;
    if (log->cancel_in_combine) {
        evio_rx_subscription_cancel(log->log.subscription);
    }
    return 0;
}

static void owned_next(void *ctx, const void *value)
{
    owned_log *log = ctx;
    const owned_number *number = value;
    assert_ptr_equal(number->self, number);
    assert_int_equal(*number->number, 11);
    log->log.count++;
    evio_rx_subscription_free(log->log.subscription);
    log->log.subscription = NULL;
    assert_ptr_equal(number->self, number);
    assert_int_equal(*number->number, 11);
    assert_int_equal(log->live, 1);
}

static void destroy_capture(void *ctx)
{
    owned_log *log = ctx;
    assert_int_equal(log->live, 0);
    log->capture_destroyed++;
}

TEST(test_evio_rx_combine_latest_owned_output_survives_callback_cancellation)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int a = 1;
    int b[] = {10, 20};
    for (size_t i = 0; i < 2; ++i) {
        owned_log data = { .cancel_in_combine = i != 0 };
        evio_rx_type type = owned_type(&data);
        evio_rx_observable *sources[] = {
            evio_rx_from_array(&int_type, &a, 1), evio_rx_from_array(&int_type, b, 2),
        };
        evio_rx_combiner combiner = {
            .type = &type,
            .cb = owned_result,
            .capture = { .data = &data, .destroy = destroy_capture },
        };
        evio_rx_observable *combined = evio_rx_combine_latest(sources, 2, combiner);
        data.log.subscription = evio_rx_subscription_new(loop, combined,
                                                         (evio_rx_observer) { .next = owned_next, .ctx = &data }, 1);
        evio_rx_observable_free(combined);
        evio_rx_observable_free(sources[0]);
        evio_rx_observable_free(sources[1]);
        assert_int_equal(data.capture_destroyed, 0);
        evio_rx_subscription_start(data.log.subscription);
        assert_int_equal(data.combine_calls, 1);
        assert_int_equal(data.created, 1);
        assert_int_equal(data.destroyed, 1);
        assert_int_equal(data.live, 0);
        assert_int_equal(data.log.count, i ? 0 : 1);
        if (i) {
            assert_true(evio_rx_subscription_is_closed(data.log.subscription));
            assert_int_equal(data.capture_destroyed, 0);
            evio_rx_subscription_free(data.log.subscription);
        } else {
            assert_null(data.log.subscription);
        }
        assert_int_equal(data.capture_destroyed, 1);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_combine_latest_invalid_configuration_keeps_capture)
{
    owned_log data = {0};
    evio_rx_combiner combiner = {
        .type = &int_type,
        .cb = sum_values,
        .capture = { .data = &data, .destroy = destroy_capture },
    };
    int value = 1;
    evio_rx_observable *sources[] = {evio_rx_from_array(&int_type, &value, 1)};
    assert_null(evio_rx_combine_latest(NULL, 1, combiner));
    assert_null(evio_rx_combine_latest(sources, 0, combiner));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_combine_latest(sources, SIZE_MAX, combiner));
    assert_int_equal(errno, EOVERFLOW);
    combiner.cb = NULL;
    assert_null(evio_rx_combine_latest(sources, 1, combiner));
    combiner.cb = sum_values;
    combiner.type = NULL;
    assert_null(evio_rx_combine_latest(sources, 1, combiner));
    assert_int_equal(errno, EINVAL);
    assert_int_equal(data.capture_destroyed, 0);
    evio_rx_observable_free(sources[0]);
}

static int count_map(void *ctx, void *out, const void *value)
{
    size_t *count = ctx;
    (*count)++;
    *(int *)out = *(const int *)value;
    return 0;
}

TEST(test_evio_rx_zip_synchronous_arrays_respect_capacity)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int a[] = {1, 2};
    int b[] = {10, 20};
    for (size_t capacity = 1; capacity <= 2; ++capacity) {
        size_t calls = 0;
        evio_rx_observable *array = evio_rx_from_array(&int_type, b, 2);
        evio_rx_observable *sources[] = {
            evio_rx_from_array(&int_type, a, 2),
            evio_rx_map(array, &int_type, count_map, (evio_rx_capture) { .data = &calls }),
        };
        evio_rx_combiner combiner = { .type = &int_type, .cb = sum_values };
        evio_rx_observable *zipped = evio_rx_zip(sources, 2, combiner, capacity);
        assert_non_null(zipped);
        combine_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, zipped, observer(&log), 1);
        assert_non_null(log.subscription);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, capacity == 1 ? 0 : 2);
        assert_int_equal(calls, capacity == 1 ? 0 : 2);
        assert_int_equal(log.completed, capacity == 1 ? 0 : 1);
        assert_int_equal(log.error, capacity == 1 ? EOVERFLOW : 0);
        if (capacity == 2) {
            int expected[] = {11, 22};
            assert_memory_equal(log.values, expected, sizeof(expected));
        }
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(zipped);
        evio_rx_observable_free(array);
        evio_rx_observable_free(sources[0]);
        evio_rx_observable_free(sources[1]);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_zip_single_input_does_not_accumulate_burst)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3};
    evio_rx_observable *input = evio_rx_from_array(&int_type, values, 3);
    evio_rx_combiner combiner = { .type = &int_type, .cb = sum_values };
    evio_rx_observable *zipped = evio_rx_zip(&input, 1, combiner, 1);
    combine_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, zipped, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.count, 3);
    assert_memory_equal(log.values, values, sizeof(values));
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(zipped);
    evio_rx_observable_free(input);
    evio_loop_free(loop);
}

TEST(test_evio_rx_zip_unequal_arrays_stop_at_shorter_input)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int a[] = {1, 2};
    int b[] = {10, 20};
    for (size_t i = 0; i < 2; ++i) {
        size_t calls = 0;
        evio_rx_observable *array = evio_rx_from_array(&int_type, b, i ? 1 : 2);
        evio_rx_observable *sources[] = {
            evio_rx_from_array(&int_type, a, i ? 2 : 1),
            evio_rx_map(array, &int_type, count_map, (evio_rx_capture) { .data = &calls }),
        };
        evio_rx_combiner combiner = { .type = &int_type, .cb = sum_values };
        evio_rx_observable *zipped = evio_rx_zip(sources, 2, combiner, 2);
        combine_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, zipped, observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, 1);
        assert_int_equal(log.values[0], 11);
        assert_int_equal(log.completed, 1);
        assert_int_equal(log.error, 0);
        assert_int_equal(calls, 1);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(zipped);
        evio_rx_observable_free(array);
        evio_rx_observable_free(sources[0]);
        evio_rx_observable_free(sources[1]);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_zip_local_completion_destroys_owned_leftovers)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    owned_log data = {0};
    evio_rx_type type = owned_type(&data);
    int numbers[] = {1, 2};
    owned_number values[] = {
        { .self = &values[0], .number = &numbers[0] },
        { .self = &values[1], .number = &numbers[1] },
    };
    int other = 10;
    evio_rx_observable *inputs[] = {
        evio_rx_from_array(&type, values, 2), evio_rx_from_array(&int_type, &other, 1),
    };
    evio_rx_combiner combiner = {
        .type = &int_type,
        .cb = owned_sum,
        .capture = { .data = &data },
    };
    evio_rx_observable *timer = evio_rx_timer(20);
    evio_rx_observable *sources[] = {
        evio_rx_zip(inputs, 2, combiner, 2),
        evio_rx_map(timer, &int_type, uint64_to_int, (evio_rx_capture) {0}),
    };
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    data.log.subscription = evio_rx_subscription_new(loop, merged, observer(&data.log), 1);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(timer);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_observable_free(inputs[0]);
    evio_rx_observable_free(inputs[1]);
    evio_rx_subscription_start(data.log.subscription);
    assert_int_equal(data.log.count, 1);
    assert_int_equal(data.log.values[0], 11);
    assert_int_equal(data.log.completed, 0);
    assert_int_equal(data.combine_calls, 1);
    assert_int_equal(data.created, 4);
    assert_int_equal(data.destroyed, 2);
    assert_int_equal(data.live, 2);
    assert_int_equal(loop->refcount, 1);
    loop->time = 120;
    evio_timer_update(loop);
    evio_invoke_pending(loop);
    assert_int_equal(data.log.count, 2);
    assert_int_equal(data.log.values[1], 0);
    assert_int_equal(data.log.completed, 1);
    assert_int_equal(data.destroyed, 2);
    assert_int_equal(data.live, 2);
    evio_rx_subscription_free(data.log.subscription);
    assert_int_equal(data.destroyed, 4);
    assert_int_equal(data.live, 0);
    evio_loop_free(loop);
}

TEST(test_evio_rx_zip_overflow_and_copy_failure_only_close_result)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        owned_log data = { .fail_copy = i ? 2 : 0 };
        evio_rx_type type = owned_type(&data);
        evio_rx_subject *a = evio_rx_subject_new(loop, &type, 1);
        evio_rx_subject *b = evio_rx_subject_new(loop, &int_type, 1);
        evio_rx_observable *sources[] = {evio_rx_from_subject(a), evio_rx_from_subject(b)};
        evio_rx_combiner combiner = {
            .type = &int_type,
            .cb = owned_sum,
            .capture = { .data = &data },
        };
        evio_rx_observable *zipped = evio_rx_zip(sources, 2, combiner, i ? 2 : 1);
        data.log.subscription = evio_rx_subscription_new(loop, zipped, observer(&data.log), 1);
        evio_rx_subscription_start(data.log.subscription);
        int number = 1;
        owned_number value = { .self = &value, .number = &number };
        assert_int_equal(evio_rx_subject_next(a, &value), 0);
        number = 2;
        assert_int_equal(evio_rx_subject_next(a, &value), 0);
        assert_int_equal(data.log.error, i ? ENOSPC : EOVERFLOW);
        assert_int_equal(data.log.completed, 0);
        assert_int_equal(data.log.count, 0);
        assert_int_equal(data.copy_calls, i ? 2 : 1);
        assert_int_equal(data.created, 1);
        assert_int_equal(data.destroyed, 1);
        assert_int_equal(data.live, 0);
        assert_int_equal(evio_rx_subject_next(a, &value), 0);
        assert_int_equal(evio_rx_subject_next(b, &number), 0);
        evio_rx_subscription_free(data.log.subscription);
        evio_rx_observable_free(zipped);
        evio_rx_observable_free(sources[0]);
        evio_rx_observable_free(sources[1]);
        evio_rx_subject_free(a);
        evio_rx_subject_free(b);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_zip_upstream_error_destroys_queued_values)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    owned_log data = {0};
    evio_rx_type type = owned_type(&data);
    int number = 1;
    owned_number value = { .self = &value, .number = &number };
    start_log producer = { .error = EIO };
    evio_rx_observable *sources[] = {
        evio_rx_from_array(&type, &value, 1),
        evio_rx_observable_new(&int_type, start_source, (evio_rx_capture) { .data = &producer }),
    };
    evio_rx_combiner combiner = {
        .type = &int_type,
        .cb = owned_sum,
        .capture = { .data = &data },
    };
    evio_rx_observable *zipped = evio_rx_zip(sources, 2, combiner, 1);
    data.log.subscription = evio_rx_subscription_new(loop, zipped, observer(&data.log), 1);
    evio_rx_observable_free(zipped);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subscription_start(data.log.subscription);
    assert_int_equal(data.log.error, EIO);
    assert_int_equal(data.log.count, 0);
    assert_int_equal(data.log.completed, 0);
    assert_int_equal(data.created, 2);
    assert_int_equal(data.destroyed, 1);
    assert_int_equal(data.live, 1);
    assert_int_equal(producer.started, 1);
    assert_int_equal(producer.stopped, 1);
    assert_int_equal(producer.destroyed, 1);
    evio_rx_subscription_free(data.log.subscription);
    assert_int_equal(data.destroyed, 2);
    assert_int_equal(data.live, 0);
    evio_loop_free(loop);
}

TEST(test_evio_rx_zip_combiner_error_destroys_current_tuple)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    owned_log data = {0};
    evio_rx_type type = owned_type(&data);
    int number = 1;
    owned_number value = { .self = &value, .number = &number };
    int b[] = {10, 20};
    evio_rx_observable *sources[] = {
        evio_rx_from_array(&type, &value, 1), evio_rx_from_array(&int_type, b, 2),
    };
    combine_failure failure = { .error = EIO };
    evio_rx_combiner combiner = {
        .type = &int_type,
        .cb = fail_combine,
        .capture = { .data = &failure },
    };
    evio_rx_observable *zipped = evio_rx_zip(sources, 2, combiner, 1);
    data.log.subscription = evio_rx_subscription_new(loop, zipped, observer(&data.log), 1);
    evio_rx_observable_free(zipped);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subscription_start(data.log.subscription);
    assert_int_equal(data.log.error, EIO);
    assert_int_equal(data.log.count, 0);
    assert_int_equal(data.log.completed, 0);
    assert_int_equal(failure.calls, 1);
    assert_int_equal(data.created, 2);
    assert_int_equal(data.destroyed, 1);
    assert_int_equal(data.live, 1);
    evio_rx_subscription_free(data.log.subscription);
    assert_int_equal(data.destroyed, 2);
    assert_int_equal(data.live, 0);
    evio_loop_free(loop);
}

TEST(test_evio_rx_zip_cancel_keeps_combiner_inputs_alive)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    owned_log data = { .cancel_in_combine = true };
    evio_rx_type type = owned_type(&data);
    int number = 1;
    owned_number value = { .self = &value, .number = &number };
    int b[] = {10, 20};
    size_t calls = 0;
    evio_rx_observable *array = evio_rx_from_array(&int_type, b, 2);
    evio_rx_observable *sources[] = {
        evio_rx_from_array(&type, &value, 1),
        evio_rx_map(array, &int_type, count_map, (evio_rx_capture) { .data = &calls }),
    };
    evio_rx_combiner combiner = {
        .type = &int_type,
        .cb = owned_sum,
        .capture = { .data = &data },
    };
    evio_rx_observable *zipped = evio_rx_zip(sources, 2, combiner, 1);
    data.log.subscription = evio_rx_subscription_new(loop, zipped, observer(&data.log), 1);
    evio_rx_observable_free(zipped);
    evio_rx_observable_free(array);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subscription_start(data.log.subscription);
    assert_int_equal(data.combine_calls, 1);
    assert_int_equal(calls, 1);
    assert_int_equal(data.log.count, 0);
    assert_int_equal(data.log.completed, 0);
    assert_int_equal(data.log.error, 0);
    assert_int_equal(data.live, 1);
    assert_int_equal(data.created, 2);
    assert_int_equal(data.destroyed, 1);
    evio_rx_subscription_free(data.log.subscription);
    assert_int_equal(data.live, 0);
    assert_int_equal(data.destroyed, 2);
    evio_loop_free(loop);
}

TEST(test_evio_rx_zip_cancel_during_copy_releases_completed_copy)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    owned_log data = { .cancel_in_copy = true };
    evio_rx_type type = owned_type(&data);
    evio_rx_subject *a = evio_rx_subject_new(loop, &type, 1);
    evio_rx_subject *b = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *sources[] = {evio_rx_from_subject(a), evio_rx_from_subject(b)};
    evio_rx_combiner combiner = {
        .type = &int_type,
        .cb = owned_sum,
        .capture = { .data = &data },
    };
    evio_rx_observable *zipped = evio_rx_zip(sources, 2, combiner, 1);
    data.log.subscription = evio_rx_subscription_new(loop, zipped, observer(&data.log), 1);
    evio_rx_subscription_start(data.log.subscription);
    int number = 1;
    owned_number value = { .self = &value, .number = &number };
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    assert_int_equal(data.combine_calls, 0);
    assert_int_equal(data.log.count, 0);
    assert_int_equal(data.live, 0);
    assert_int_equal(data.created, 1);
    assert_int_equal(data.destroyed, 1);
    assert_true(evio_rx_subscription_is_closed(data.log.subscription));
    evio_rx_subscription_free(data.log.subscription);
    evio_rx_observable_free(zipped);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(a);
    evio_rx_subject_free(b);
    evio_loop_free(loop);
}

TEST(test_evio_rx_zip_free_from_downstream_keeps_owned_output_alive)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    owned_log data = {0};
    evio_rx_type type = owned_type(&data);
    int a[] = {1, 2};
    int b[] = {10, 20};
    size_t calls = 0;
    evio_rx_observable *array = evio_rx_from_array(&int_type, b, 2);
    evio_rx_observable *sources[] = {
        evio_rx_from_array(&int_type, a, 2),
        evio_rx_map(array, &int_type, count_map, (evio_rx_capture) { .data = &calls }),
    };
    evio_rx_combiner combiner = {
        .type = &type,
        .cb = owned_result,
        .capture = { .data = &data, .destroy = destroy_capture },
    };
    evio_rx_observable *zipped = evio_rx_zip(sources, 2, combiner, 2);
    data.log.subscription = evio_rx_subscription_new(loop, zipped,
                                                     (evio_rx_observer) { .next = owned_next, .ctx = &data }, 1);
    evio_rx_observable_free(zipped);
    evio_rx_observable_free(array);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subscription_start(data.log.subscription);
    assert_null(data.log.subscription);
    assert_int_equal(data.combine_calls, 1);
    assert_int_equal(calls, 1);
    assert_int_equal(data.log.count, 1);
    assert_int_equal(data.created, 1);
    assert_int_equal(data.destroyed, 1);
    assert_int_equal(data.live, 0);
    assert_int_equal(data.capture_destroyed, 1);
    evio_loop_free(loop);
}

TEST(test_evio_rx_zip_rejects_invalid_capacity_and_keeps_capture)
{
    owned_log data = {0};
    evio_rx_combiner combiner = {
        .type = &int_type,
        .cb = sum_values,
        .capture = { .data = &data, .destroy = destroy_capture },
    };
    int number = 1;
    evio_rx_observable *sources[] = {evio_rx_from_array(&int_type, &number, 1)};
    assert_null(evio_rx_zip(sources, 1, combiner, 0));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_zip(sources, 1, combiner, SIZE_MAX));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_zip(sources, SIZE_MAX, combiner, 1));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_zip(NULL, 1, combiner, 1));
    assert_null(evio_rx_zip(sources, 0, combiner, 1));
    assert_int_equal(errno, EINVAL);
    assert_int_equal(data.capture_destroyed, 0);
    evio_rx_observable_free(sources[0]);
}

TEST(test_evio_rx_take_until_synchronous_notifier_precedes_source)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2};
    uint64_t notification = 7;
    for (size_t count = 0; count < 2; ++count) {
        size_t calls = 0;
        evio_rx_observable *array = evio_rx_from_array(&int_type, values, 2);
        evio_rx_observable *source = evio_rx_map(array, &int_type, count_map,
                                                 (evio_rx_capture) { .data = &calls });
        evio_rx_observable *notifier = evio_rx_from_array(&evio_rx_uint64_type, &notification, count);
        evio_rx_observable *limited = evio_rx_take_until(source, notifier);
        assert_non_null(limited);
        combine_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, limited, observer(&log), 1);
        assert_non_null(log.subscription);
        evio_rx_observable_free(array);
        evio_rx_observable_free(source);
        evio_rx_observable_free(notifier);
        evio_rx_observable_free(limited);
        assert_int_equal(calls, 0);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(calls, count ? 0 : 2);
        assert_int_equal(log.count, count ? 0 : 2);
        if (!count) {
            assert_memory_equal(log.values, values, sizeof(values));
        }
        assert_int_equal(log.completed, 1);
        assert_int_equal(log.error, 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_take_until_notifier_error_does_not_start_source)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    start_log source_log = {0};
    start_log notifier_log = { .error = EIO };
    evio_rx_observable *source = evio_rx_observable_new(&int_type, start_source,
                                                        (evio_rx_capture) { .data = &source_log });
    evio_rx_observable *notifier = evio_rx_observable_new(&int_type, start_source,
                                                          (evio_rx_capture) { .data = &notifier_log });
    evio_rx_observable *limited = evio_rx_take_until(source, notifier);
    combine_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, limited, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, EIO);
    assert_int_equal(source_log.started, 0);
    assert_int_equal(source_log.destroyed, 1);
    assert_int_equal(notifier_log.started, 1);
    assert_int_equal(notifier_log.stopped, 1);
    assert_int_equal(notifier_log.destroyed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(notifier);
    evio_rx_observable_free(limited);
    evio_loop_free(loop);
}

TEST(test_evio_rx_take_until_source_terminal_or_cancel_stops_notifier)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 3; ++i) {
        loop->time = 100;
        start_log source_log = { .error = i == 1 ? EIO : 0 };
        evio_rx_observable *source = evio_rx_observable_new(&int_type, start_source,
                                                            (evio_rx_capture) { .data = &source_log });
        evio_rx_observable *notifier = evio_rx_timer(10);
        evio_rx_observable *limited = evio_rx_take_until(source, notifier);
        combine_log log = { .cancel = i == 2 };
        log.subscription = evio_rx_subscription_new(loop, limited, observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, i == 1 ? 0 : 1);
        assert_int_equal(log.completed, i == 0 ? 1 : 0);
        assert_int_equal(log.error, i == 1 ? EIO : 0);
        assert_int_equal(source_log.started, 1);
        assert_int_equal(source_log.stopped, 1);
        assert_int_equal(source_log.destroyed, 1);
        assert_int_equal(loop->refcount, 0);
        loop->time = 110;
        evio_timer_update(loop);
        assert_int_equal(evio_pending_count(loop), 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(notifier);
        evio_rx_observable_free(limited);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_take_until_simultaneously_pending_timers_stop_both)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_observable *timer = evio_rx_timer(0);
    evio_rx_observable *source = evio_rx_map(timer, &int_type, uint64_to_int, (evio_rx_capture) {0});
    evio_rx_observable *notifier = evio_rx_timer(0);
    evio_rx_observable *limited = evio_rx_take_until(source, notifier);
    combine_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, limited, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(loop->refcount, 2);
    assert_int_equal(log.count, 0);
    evio_timer_update(loop);
    assert_int_equal(evio_pending_count(loop), 2);
    evio_invoke_pending(loop);
    assert_true(log.count <= 1);
    if (log.count) {
        assert_int_equal(log.values[0], 0);
    }
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(loop->refcount, 0);
    assert_int_equal(evio_pending_count(loop), 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(timer);
    evio_rx_observable_free(source);
    evio_rx_observable_free(notifier);
    evio_rx_observable_free(limited);
    evio_loop_free(loop);
}

TEST(test_evio_rx_take_until_subject_notification_only_cancels_membership)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *a = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_subject *b = evio_rx_subject_new(loop, &evio_rx_uint64_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(a);
    evio_rx_observable *notifier = evio_rx_from_subject(b);
    evio_rx_observable *limited = evio_rx_take_until(source, notifier);
    combine_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, limited, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    uint64_t notification = 2;
    assert_int_equal(evio_rx_subject_next(b, &notification), 0);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.values[0], 1);
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(evio_rx_subject_next(a, &value), 0);
    assert_int_equal(evio_rx_subject_next(b, &notification), 0);
    assert_int_equal(log.count, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(notifier);
    evio_rx_observable_free(limited);
    evio_rx_subject_free(a);
    evio_rx_subject_free(b);
    evio_loop_free(loop);
}

TEST(test_evio_rx_take_until_rejects_null_and_loop_mismatch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_loop *other = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *a = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_subject *b = evio_rx_subject_new(other, &evio_rx_uint64_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(a);
    evio_rx_observable *notifier = evio_rx_from_subject(b);
    assert_null(evio_rx_take_until(source, NULL));
    assert_null(evio_rx_take_until(NULL, notifier));
    assert_null(evio_rx_take_until(source, notifier));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable_free(source);
    evio_rx_observable_free(notifier);
    evio_rx_subject_free(a);
    evio_rx_subject_free(b);
    evio_loop_free(loop);
    evio_loop_free(other);
}

TEST(test_evio_rx_merge_rejects_type_identity_and_loop_mismatch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_loop *other = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_type similar = int_type;
    int value = 1;
    evio_rx_observable *sources[] = {
        evio_rx_from_array(&int_type, &value, 1), evio_rx_from_array(&similar, &value, 1),
    };
    assert_null(evio_rx_merge(NULL, 1));
    assert_null(evio_rx_merge(sources, 0));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_merge(sources, SIZE_MAX));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_merge(sources, 2));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable_free(sources[1]);
    sources[1] = NULL;
    assert_null(evio_rx_merge(sources, 2));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable_free(sources[0]);
    evio_rx_subject *a = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_subject *b = evio_rx_subject_new(other, &int_type, 1);
    sources[0] = evio_rx_from_subject(a);
    sources[1] = evio_rx_from_subject(b);
    assert_null(evio_rx_merge(sources, 2));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(a);
    evio_rx_subject_free(b);
    evio_loop_free(other);
    evio_loop_free(loop);
}

static int count_batch(void *ctx, void *out, const void *values, size_t count)
{
    size_t *calls = ctx;
    (*calls)++;
    memcpy(out, values, count * sizeof(int));
    return 0;
}

TEST(test_evio_rx_merge_forwards_input_batches)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int a[] = {1, 2, 3};
    int b[] = {4, 5};
    evio_rx_observable *sources[] = {
        evio_rx_from_array(&int_type, a, 3), evio_rx_from_array(&int_type, b, 2),
    };
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    size_t calls = 0;
    evio_rx_observable *mapped = evio_rx_map_batch(merged, &int_type, count_batch,
                                                   (evio_rx_capture) { .data = &calls });
    for (int cancel = 0; cancel < 2; ++cancel) {
        calls = 0;
        combine_log log = { .cancel = cancel };
        log.subscription = evio_rx_subscription_new(loop, mapped, observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        int expected[] = {1, 2, 3, 4, 5};
        assert_int_equal(calls, cancel ? 1 : 2);
        assert_int_equal(log.count, cancel ? 1 : 5);
        assert_memory_equal(log.values, expected, log.count * sizeof(int));
        assert_int_equal(log.completed, !cancel);
        evio_rx_subscription_free(log.subscription);
    }
    evio_rx_observable_free(mapped);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_loop_free(loop);
}

static int keep_none(void *ctx, void *out, size_t *kept, const void *values, size_t count)
{
    *kept = 0;
    return 0;
}

TEST(test_evio_rx_take_until_ignores_empty_notifier_batch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3};
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *notifier = evio_rx_filter_batch(array, keep_none, (evio_rx_capture) {0});
    evio_rx_observable *taken = evio_rx_take_until(source, notifier);
    combine_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, taken, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.count, 3);
    assert_memory_equal(log.values, values, sizeof(values));
    assert_int_equal(log.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(taken);
    evio_rx_observable_free(notifier);
    evio_rx_observable_free(array);
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

TEST(test_evio_rx_take_until_notifier_batch_completes)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    int values[] = {1, 2, 3};
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *notifier = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *taken = evio_rx_take_until(source, notifier);
    combine_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, taken, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(taken);
    evio_rx_observable_free(notifier);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_take_until_notifier_during_batch_follows_it)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    int values[] = {1, 2, 3};
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *notifier = evio_rx_from_subject(subject);
    evio_rx_observable *taken = evio_rx_take_until(source, notifier);
    size_t calls = 0;
    evio_rx_observable *mapped = evio_rx_map_batch(taken, &int_type, count_batch,
                                                   (evio_rx_capture) { .data = &calls });
    combine_log log = { .other = subject, .action = publish_other };
    log.subscription = evio_rx_subscription_new(loop, mapped, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(calls, 1);
    assert_int_equal(log.count, 3);
    assert_memory_equal(log.values, values, sizeof(values));
    assert_int_equal(log.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(mapped);
    evio_rx_observable_free(taken);
    evio_rx_observable_free(notifier);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}
