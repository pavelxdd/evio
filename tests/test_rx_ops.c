#include "test.h"
#include "evio_rx.h"

static const evio_rx_type int_type = {
    .size = sizeof(int), .alignment = alignof(int),
};

typedef struct {
    evio_rx_subscription *subscription;
    int values[16];
    size_t count;
    size_t completed;
    int error;
    size_t calls;
    size_t copies;
    size_t destroyed;
    bool reject;
    bool cancel;
} operator_log;

static void record_next(void *ctx, const void *value)
{
    operator_log *log = ctx;
    assert_true(log->count < 16);
    log->values[log->count++] = *(const int *)value;
    if (log->cancel) {
        evio_rx_subscription_cancel(log->subscription);
    }
}

static void record_error(void *ctx, int error)
{
    operator_log *log = ctx;
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
    log->error = error;
}

static void record_complete(void *ctx)
{
    operator_log *log = ctx;
    assert_int_equal(log->completed, 0);
    assert_int_equal(log->error, 0);
    log->completed++;
}

static evio_rx_observer observer(operator_log *log)
{
    return (evio_rx_observer) {
        .next = record_next, .error = record_error, .complete = record_complete, .ctx = log,
    };
}

static bool odd(void *ctx, const void *value)
{
    operator_log *log = ctx;
    log->calls++;
    return !log->reject && (*(const int *)value % 2 != 0);
}

static int count_copy(void *ctx, void *out, const void *value)
{
    operator_log *log = ctx;
    log->copies++;
    *(int *)out = *(const int *)value;
    return 0;
}

static void count_destroy(void *ctx, void *value)
{
    operator_log *log = ctx;
    log->destroyed++;
}

TEST(test_evio_rx_filter_borrows_values_and_propagates_complete)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3};
    for (size_t i = 0; i < 2; ++i) {
        operator_log log = { .reject = i };
        evio_rx_type type = {
            .size = sizeof(int), .alignment = alignof(int), .ctx = &log,
            .copy = count_copy, .destroy = count_destroy,
        };
        evio_rx_observable *array = evio_rx_from_array(&type, values, 3);
        evio_rx_observable *filtered = evio_rx_filter(array, odd,
                                                      (evio_rx_capture) { .data = &log });
        assert_non_null(filtered);
        log.subscription = evio_rx_subscription_new(loop, filtered, observer(&log), 1);
        assert_non_null(log.subscription);
        evio_rx_observable_free(array);
        evio_rx_observable_free(filtered);
        assert_int_equal(log.copies, 3);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.calls, 3);
        assert_int_equal(log.count, i ? 0 : 2);
        if (!i) {
            assert_int_equal(log.values[0], 1);
            assert_int_equal(log.values[1], 3);
        }
        assert_int_equal(log.completed, 1);
        assert_int_equal(log.error, 0);
        assert_int_equal(log.copies, 3);
        assert_int_equal(log.destroyed, 0);
        evio_rx_subscription_free(log.subscription);
        assert_int_equal(log.destroyed, 3);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_filter_downstream_cancel_stops_predicate)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    operator_log log = { .cancel = true };
    int values[] = {1, 2, 3};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *filtered = evio_rx_filter(array, odd, (evio_rx_capture) { .data = &log });
    log.subscription = evio_rx_subscription_new(loop, filtered, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.calls, 1);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.values[0], 1);
    assert_int_equal(log.completed, 0);
    assert_true(evio_rx_subscription_is_closed(log.subscription));
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(filtered);
    evio_rx_observable_free(array);
    evio_loop_free(loop);
}

typedef struct {
    int value;
    const void *self;
} sum_value;

typedef struct {
    size_t attempts;
    size_t created;
    size_t destroyed;
    size_t live;
    size_t fail_at;
    size_t calls;
    size_t captures;
    int fail_value;
} sum_log;

static int sum_copy(void *ctx, void *out, const void *value)
{
    sum_log *log = ctx;
    const sum_value *input = value;
    assert_ptr_equal(input->self, input);
    if (++log->attempts == log->fail_at) {
        return EIO;
    }
    sum_value *result = out;
    result->value = input->value;
    result->self = result;
    log->created++;
    log->live++;
    return 0;
}

static void sum_destroy(void *ctx, void *value)
{
    sum_log *log = ctx;
    sum_value *input = value;
    assert_ptr_equal(input->self, input);
    assert_true(log->live > 0);
    log->live--;
    log->destroyed++;
}

static void sum_capture_destroy(void *ctx)
{
    sum_log *log = ctx;
    log->captures++;
}

static int sum_reduce(void *ctx, void *out, const void *accumulator, const void *value)
{
    sum_log *log = ctx;
    const sum_value *previous = accumulator;
    assert_ptr_equal(previous->self, previous);
    log->calls++;
    int input = *(const int *)value;
    if (input == log->fail_value) {
        return EIO;
    }
    sum_value *result = out;
    result->value = previous->value + input;
    result->self = result;
    log->created++;
    log->live++;
    return 0;
}

static void sum_next(void *ctx, const void *value)
{
    const sum_value *input = value;
    assert_ptr_equal(input->self, input);
    record_next(ctx, &input->value);
    assert_ptr_equal(input->self, input);
}

TEST(test_evio_rx_scan_independent_owned_accumulators)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    sum_log sums = {0};
    evio_rx_type type = {
        .size = sizeof(sum_value), .alignment = alignof(sum_value),
        .copy = sum_copy, .destroy = sum_destroy, .ctx = &sums,
    };
    sum_value seed = { .value = 0 };
    seed.self = &seed;
    int values[] = {1, 2, 3};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *scanned = evio_rx_scan(array, &type, &seed, sum_reduce,
                                               (evio_rx_capture) { .data = &sums });
    assert_non_null(scanned);
    operator_log logs[2] = {0};
    for (size_t i = 0; i < 2; ++i) {
        evio_rx_observer callbacks = observer(&logs[i]);
        callbacks.next = sum_next;
        logs[i].subscription = evio_rx_subscription_new(loop, scanned, callbacks, 1);
        assert_non_null(logs[i].subscription);
    }
    evio_rx_observable_free(array);
    evio_rx_observable_free(scanned);
    assert_int_equal(sums.live, 3);
    for (size_t i = 0; i < 2; ++i) {
        evio_rx_subscription_start(logs[i].subscription);
        int expected[] = {1, 3, 6};
        assert_int_equal(logs[i].count, 3);
        assert_memory_equal(logs[i].values, expected, sizeof(expected));
        assert_int_equal(logs[i].completed, 1);
        evio_rx_subscription_free(logs[i].subscription);
    }
    assert_int_equal(sums.calls, 6);
    assert_int_equal(sums.live, 0);
    assert_int_equal(sums.created, 9);
    assert_int_equal(sums.destroyed, sums.created);
    evio_loop_free(loop);
}

TEST(test_evio_rx_scan_restart_copies_seed_again)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    sum_log sums = {0};
    evio_rx_type type = {
        .size = sizeof(sum_value), .alignment = alignof(sum_value),
        .copy = sum_copy, .destroy = sum_destroy, .ctx = &sums,
    };
    sum_value seed = { .value = 0 };
    seed.self = &seed;
    int values[] = {1, 2, 3};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *scanned = evio_rx_scan(array, &type, &seed, sum_reduce,
                                               (evio_rx_capture) { .data = &sums });
    operator_log log = {0};
    evio_rx_observer callbacks = { .next = sum_next, .ctx = &log };
    log.subscription = evio_rx_subscription_new(loop, scanned, callbacks, 1);
    assert_non_null(log.subscription);
    evio_rx_observable_free(array);
    evio_rx_observable_free(scanned);
    evio_rx_subscription_start(log.subscription);
    evio_rx_subscription_restart(log.subscription);
    int expected[] = {1, 3, 6, 1, 3, 6};
    assert_int_equal(log.count, 6);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_true(evio_rx_subscription_is_closed(log.subscription));
    assert_int_equal(sums.live, 1);
    evio_rx_subscription_free(log.subscription);
    assert_int_equal(sums.live, 0);
    assert_int_equal(sums.destroyed, sums.created);
    evio_loop_free(loop);
}

TEST(test_evio_rx_scan_restart_seed_copy_failure_errors_run)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    sum_log sums = {0};
    evio_rx_type type = {
        .size = sizeof(sum_value), .alignment = alignof(sum_value),
        .copy = sum_copy, .destroy = sum_destroy, .ctx = &sums,
    };
    sum_value seed = { .value = 0 };
    seed.self = &seed;
    int values[] = {1, 2};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 2);
    evio_rx_observable *scanned = evio_rx_scan(array, &type, &seed, sum_reduce,
                                               (evio_rx_capture) { .data = &sums });
    operator_log log = {0};
    evio_rx_observer callbacks = observer(&log);
    callbacks.next = sum_next;
    log.subscription = evio_rx_subscription_new(loop, scanned, callbacks, 1);
    evio_rx_observable_free(array);
    evio_rx_observable_free(scanned);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.completed, 1);
    sums.fail_at = sums.attempts + 1;
    log.completed = 0;
    evio_rx_subscription_restart(log.subscription);
    assert_int_equal(log.count, 2);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, EIO);
    assert_int_equal(sums.calls, 2);
    evio_rx_subscription_free(log.subscription);
    assert_int_equal(sums.live, 0);
    assert_int_equal(sums.destroyed, sums.created);
    evio_loop_free(loop);
}

TEST(test_evio_rx_scan_seed_copy_failure_does_not_notify_or_take_capture)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    sum_log sums = { .fail_at = 1 };
    evio_rx_type type = {
        .size = sizeof(sum_value), .alignment = alignof(sum_value),
        .copy = sum_copy, .destroy = sum_destroy, .ctx = &sums,
    };
    evio_rx_capture capture = { .data = &sums, .destroy = sum_capture_destroy };
    sum_value seed = { .value = 0 };
    seed.self = &seed;
    int value = 1;
    evio_rx_observable *array = evio_rx_from_array(&int_type, &value, 1);
    assert_null(evio_rx_scan(array, &type, &seed, sum_reduce, capture));
    assert_int_equal(errno, EIO);
    assert_int_equal(sums.captures, 0);
    assert_int_equal(sums.live, 0);
    evio_rx_observable *scanned = evio_rx_scan(array, &type, &seed, sum_reduce, capture);
    assert_non_null(scanned);
    sums.fail_at = sums.attempts + 1;
    operator_log log = {0};
    assert_null(evio_rx_subscription_new(loop, scanned, observer(&log), 1));
    assert_int_equal(errno, EIO);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.error, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(sums.live, 1);
    assert_int_equal(sums.calls, 0);
    assert_int_equal(sums.captures, 0);
    evio_rx_observable_free(scanned);
    assert_int_equal(sums.captures, 1);
    assert_int_equal(sums.live, 0);
    assert_int_equal(sums.destroyed, sums.created);
    evio_rx_observable_free(array);
    evio_loop_free(loop);
}

TEST(test_evio_rx_scan_empty_error_and_downstream_cancel_release_values)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 3; ++i) {
        sum_log sums = { .fail_value = i == 1 ? 2 : 0 };
        evio_rx_type type = {
            .size = sizeof(sum_value), .alignment = alignof(sum_value),
            .copy = sum_copy, .destroy = sum_destroy, .ctx = &sums,
        };
        sum_value seed = { .value = 0 };
        seed.self = &seed;
        int values[] = {1, 2, 3};
        evio_rx_observable *array = evio_rx_from_array(&int_type, values, i ? 3 : 0);
        evio_rx_observable *scanned = evio_rx_scan(array, &type, &seed, sum_reduce,
                                                   (evio_rx_capture) { .data = &sums });
        operator_log log = { .cancel = i == 2 };
        evio_rx_observer callbacks = observer(&log);
        callbacks.next = sum_next;
        log.subscription = evio_rx_subscription_new(loop, scanned, callbacks, 1);
        assert_non_null(log.subscription);
        evio_rx_observable_free(array);
        evio_rx_observable_free(scanned);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, i ? 1 : 0);
        assert_int_equal(log.completed, !i);
        assert_int_equal(log.error, i == 1 ? EIO : 0);
        assert_int_equal(sums.calls, i == 1 ? 2 : (i == 2 ? 1 : 0));
        assert_int_equal(sums.live, 1);
        evio_rx_subscription_free(log.subscription);
        assert_int_equal(sums.live, 0);
        assert_int_equal(sums.destroyed, sums.created);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_scan_rejects_invalid_configuration)
{
    int value = 1;
    evio_rx_observable *array = evio_rx_from_array(&int_type, &value, 1);
    assert_null(evio_rx_scan(NULL, &int_type, &value, sum_reduce, (evio_rx_capture) {0}));
    assert_null(evio_rx_scan(array, NULL, &value, sum_reduce, (evio_rx_capture) {0}));
    assert_null(evio_rx_scan(array, &int_type, NULL, sum_reduce, (evio_rx_capture) {0}));
    assert_null(evio_rx_scan(array, &int_type, &value, NULL, (evio_rx_capture) {0}));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable_free(array);
}

static bool int_equal(void *ctx, const void *previous, const void *value)
{
    operator_log *log = ctx;
    log->calls++;
    return *(const int *)previous == *(const int *)value;
}

TEST(test_evio_rx_distinct_last_emitted_value)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 1, 2, 2, 1};
    operator_log log = {0};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 5);
    evio_rx_observable *distinct = evio_rx_distinct_until_changed(array, int_equal,
                                                                  (evio_rx_capture) { .data = &log });
    assert_non_null(distinct);
    log.subscription = evio_rx_subscription_new(loop, distinct, observer(&log), 1);
    evio_rx_observable_free(array);
    evio_rx_observable_free(distinct);
    evio_rx_subscription_start(log.subscription);
    int expected[] = {1, 2, 1};
    assert_int_equal(log.count, 3);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(log.calls, 4);
    assert_int_equal(log.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_loop_free(loop);
}

typedef struct {
    evio_rx_emitter *emitter;
    size_t copies;
    size_t destroyed;
    int fail_value;
    bool reenter;
} distinct_producer;

typedef struct {
    distinct_producer *log;
} distinct_state;

static int distinct_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    distinct_state *producer = evio_calloc(1, sizeof(*producer));
    producer->log = ctx;
    producer->log->emitter = emitter;
    *state = producer;
    return 0;
}

static void distinct_start(evio_rx_emitter *emitter, void *state)
{
}

static void distinct_stop(void *state)
{
    distinct_state *producer = state;
    producer->log->emitter = NULL;
}

static void distinct_destroy(void *state)
{
    evio_free(state);
}

static void distinct_emit(evio_rx_emitter *emitter, void *state, void *data)
{
    evio_rx_emitter_next(emitter, data);
}

static int distinct_copy(void *ctx, void *out, const void *value)
{
    distinct_producer *log = ctx;
    int input = *(const int *)value;
    if (input == log->fail_value) {
        return EIO;
    }
    *(int *)out = input;
    log->copies++;
    if (log->reenter) {
        log->reenter = false;
        int next = 2;
        evio_rx_emitter_invoke(log->emitter, distinct_emit, &next);
        next = 99;
        assert_int_equal(*(int *)out, input);
    }
    return 0;
}

static void distinct_value_destroy(void *ctx, void *value)
{
    distinct_producer *log = ctx;
    log->destroyed++;
}

TEST(test_evio_rx_distinct_owned_previous_reentry_and_copy_failure)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 3; ++i) {
        distinct_producer producer = { .reenter = i == 1, .fail_value = i == 2 ? 2 : 0 };
        evio_rx_type type = {
            .size = sizeof(int), .alignment = alignof(int), .ctx = &producer,
            .copy = distinct_copy, .destroy = distinct_value_destroy,
        };
        evio_rx_source hooks = {
            .prepare = distinct_prepare, .start = distinct_start,
            .stop = distinct_stop, .destroy = distinct_destroy,
        };
        operator_log log = {0};
        evio_rx_observable *source = evio_rx_observable_new(&type, hooks,
                                                            (evio_rx_capture) { .data = &producer });
        evio_rx_observable *distinct = evio_rx_distinct_until_changed(source, int_equal,
                                                                      (evio_rx_capture) { .data = &log });
        log.subscription = evio_rx_subscription_new(loop, distinct, observer(&log), 2);
        assert_non_null(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(distinct);
        evio_rx_subscription_start(log.subscription);
        int value = 1;
        evio_rx_emitter_invoke(producer.emitter, distinct_emit, &value);
        value = 99;
        if (!i) {
            value = 1;
            evio_rx_emitter_invoke(producer.emitter, distinct_emit, &value);
            assert_int_equal(log.count, 1);
        }
        if (i != 1) {
            value = 2;
            evio_rx_emitter_invoke(producer.emitter, distinct_emit, &value);
        }
        assert_int_equal(log.count, i == 2 ? 1 : 2);
        assert_int_equal(log.values[0], 1);
        if (i != 2) {
            assert_int_equal(log.values[1], 2);
        }
        assert_int_equal(log.error, i == 2 ? EIO : 0);
        evio_rx_subscription_free(log.subscription);
        assert_int_equal(producer.copies, i == 1 ? 3 : (i == 2 ? 1 : 2));
        assert_int_equal(producer.destroyed, producer.copies);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_distinct_rejects_invalid_configuration)
{
    int value = 1;
    assert_null(evio_rx_distinct_until_changed(NULL, int_equal, (evio_rx_capture) {0}));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable *array = evio_rx_from_array(&int_type, &value, 1);
    assert_null(evio_rx_distinct_until_changed(array, NULL, (evio_rx_capture) {0}));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable_free(array);
}

TEST(test_evio_rx_filter_rejects_invalid_configuration)
{
    int value = 1;
    operator_log log = {0};
    assert_null(evio_rx_filter(NULL, odd, (evio_rx_capture) { .data = &log }));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable *source = evio_rx_from_array(&int_type, &value, 1);
    assert_null(evio_rx_filter(source, NULL, (evio_rx_capture) { .data = &log }));
    assert_int_equal(errno, EINVAL);
    assert_int_equal(log.calls, 0);
    evio_rx_observable_free(source);
}

typedef struct {
    int value;
    char padding[124];
} large_int;

static int twice_batch(void *ctx, void *out, const void *values, size_t count)
{
    operator_log *log = ctx;
    log->calls++;
    if (log->reject) {
        return EIO;
    }
    int *result = out;
    const int *value = values;
    for (size_t i = 0; i < count; ++i) {
        result[i] = value[i] * 2;
    }
    return 0;
}

static int widen_batch(void *ctx, void *out, const void *values, size_t count)
{
    operator_log *log = ctx;
    log->calls++;
    large_int *result = out;
    const int *value = values;
    for (size_t i = 0; i < count; ++i) {
        result[i] = (large_int) { .value = value[i] * 2 };
    }
    return 0;
}

static int narrow_batch(void *ctx, void *out, const void *values, size_t count)
{
    operator_log *log = ctx;
    log->calls++;
    int *result = out;
    const large_int *value = values;
    for (size_t i = 0; i < count; ++i) {
        result[i] = value[i].value * 2;
    }
    return 0;
}

static int odd_batch(void *ctx, void *out, size_t *kept, const void *values, size_t count)
{
    operator_log *log = ctx;
    log->calls++;
    if (log->reject) {
        return EIO;
    }
    int *result = out;
    const int *value = values;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        if (value[i] % 2 != 0) {
            result[n++] = value[i];
        }
    }
    *kept = n;
    return 0;
}

TEST(test_evio_rx_map_batch_transforms_array_in_one_call)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    operator_log log = {0};
    int values[] = {1, 2, 3};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *mapped = evio_rx_map_batch(array, &int_type, twice_batch,
                                                   (evio_rx_capture) { .data = &log });
    assert_non_null(mapped);
    log.subscription = evio_rx_subscription_new(loop, mapped, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.calls, 1);
    assert_int_equal(log.count, 3);
    assert_int_equal(log.values[0], 2);
    assert_int_equal(log.values[1], 4);
    assert_int_equal(log.values[2], 6);
    assert_int_equal(log.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(mapped);
    evio_rx_observable_free(array);
    evio_loop_free(loop);
}

TEST(test_evio_rx_map_batch_splits_large_output_values)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    operator_log log = {0};
    evio_rx_type type = { .size = sizeof(large_int), .alignment = alignof(large_int) };
    int values[] = {1, 2, 3, 4, 5};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 5);
    evio_rx_observable *mapped = evio_rx_map_batch(array, &type, widen_batch,
                                                   (evio_rx_capture) { .data = &log });
    evio_rx_observable *ints = evio_rx_map_batch(mapped, &int_type, narrow_batch,
                                                 (evio_rx_capture) { .data = &log });
    log.subscription = evio_rx_subscription_new(loop, ints, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.count, 5);
    for (size_t i = 0; i < 5; ++i) {
        assert_int_equal(log.values[i], (int)(i + 1) * 4);
    }
    assert_int_equal(log.calls, 6);
    assert_int_equal(log.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(ints);
    evio_rx_observable_free(mapped);
    evio_rx_observable_free(array);
    evio_loop_free(loop);
}

TEST(test_evio_rx_batch_operator_failure_fails_run)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 3);
    for (size_t i = 0; i < 2; ++i) {
        operator_log log = { .reject = true };
        evio_rx_capture capture = { .data = &log };
        evio_rx_observable *batched = i ? evio_rx_filter_batch(array, odd_batch, capture)
                                      : evio_rx_map_batch(array, &int_type, twice_batch, capture);
        log.subscription = evio_rx_subscription_new(loop, batched, observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.calls, 1);
        assert_int_equal(log.count, 0);
        assert_int_equal(log.error, EIO);
        assert_true(evio_rx_subscription_is_closed(log.subscription));
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(batched);
    }
    evio_rx_observable_free(array);
    evio_loop_free(loop);
}

TEST(test_evio_rx_map_batch_downstream_cancel_destroys_built_values)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    operator_log log = { .cancel = true };
    evio_rx_type type = {
        .size = sizeof(int), .alignment = alignof(int), .ctx = &log,
        .copy = count_copy, .destroy = count_destroy,
    };
    int values[] = {1, 2, 3};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *mapped = evio_rx_map_batch(array, &type, twice_batch,
                                                   (evio_rx_capture) { .data = &log });
    log.subscription = evio_rx_subscription_new(loop, mapped, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.values[0], 2);
    assert_int_equal(log.destroyed, 3);
    assert_int_equal(log.completed, 0);
    assert_true(evio_rx_subscription_is_closed(log.subscription));
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(mapped);
    evio_rx_observable_free(array);
    evio_loop_free(loop);
}

TEST(test_evio_rx_filter_batch_keeps_selected_values_in_order)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    operator_log log = {0};
    int values[] = {1, 2, 3, 4, 5};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 5);
    evio_rx_observable *filtered = evio_rx_filter_batch(array, odd_batch,
                                                        (evio_rx_capture) { .data = &log });
    assert_non_null(filtered);
    log.subscription = evio_rx_subscription_new(loop, filtered, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.calls, 1);
    assert_int_equal(log.count, 3);
    assert_int_equal(log.values[0], 1);
    assert_int_equal(log.values[1], 3);
    assert_int_equal(log.values[2], 5);
    assert_int_equal(log.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(filtered);
    evio_rx_observable_free(array);
    evio_loop_free(loop);
}

TEST(test_evio_rx_batch_operators_take_subject_values_one_at_a_time)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    operator_log log = {0};
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *input = evio_rx_from_subject(subject);
    evio_rx_observable *filtered = evio_rx_filter_batch(input, odd_batch,
                                                        (evio_rx_capture) { .data = &log });
    evio_rx_observable *mapped = evio_rx_map_batch(filtered, &int_type, twice_batch,
                                                   (evio_rx_capture) { .data = &log });
    log.subscription = evio_rx_subscription_new(loop, mapped, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    for (int value = 1; value <= 3; ++value) {
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    }
    evio_rx_subject_complete(subject);
    assert_int_equal(log.calls, 5);
    assert_int_equal(log.count, 2);
    assert_int_equal(log.values[0], 2);
    assert_int_equal(log.values[1], 6);
    assert_int_equal(log.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(mapped);
    evio_rx_observable_free(filtered);
    evio_rx_observable_free(input);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_batch_operators_reject_invalid_configuration)
{
    int value = 1;
    evio_rx_type invalid = { .size = 0, .alignment = 1 };
    evio_rx_observable *source = evio_rx_from_array(&int_type, &value, 1);
    assert_null(evio_rx_map_batch(NULL, &int_type, twice_batch, (evio_rx_capture) {0}));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_map_batch(source, &invalid, twice_batch, (evio_rx_capture) {0}));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_map_batch(source, &int_type, NULL, (evio_rx_capture) {0}));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_filter_batch(NULL, odd_batch, (evio_rx_capture) {0}));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_filter_batch(source, NULL, (evio_rx_capture) {0}));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable_free(source);
}

TEST(test_evio_rx_operators_reject_oversized_value_type)
{
    int value = 1;
    evio_rx_type huge = { .size = SIZE_MAX, .alignment = 1 };
    evio_rx_observable *source = evio_rx_from_array(&int_type, &value, 1);
    evio_rx_observable *huge_source = evio_rx_from_array(&huge, NULL, 0);
    assert_non_null(huge_source);
    assert_null(evio_rx_map(source, &huge, count_copy, (evio_rx_capture) {0}));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_map_batch(source, &huge, twice_batch, (evio_rx_capture) {0}));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_scan(source, &huge, &value, sum_reduce, (evio_rx_capture) {0}));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_distinct_until_changed(huge_source, int_equal, (evio_rx_capture) {0}));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_filter_batch(huge_source, odd_batch, (evio_rx_capture) {0}));
    assert_int_equal(errno, EOVERFLOW);
    evio_rx_observable_free(huge_source);
    evio_rx_observable_free(source);
}
