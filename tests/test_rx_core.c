#include "test.h"
#include "evio_rx.h"

static const evio_rx_type int_type = {
    .size = sizeof(int),
    .alignment = alignof(int),
};

typedef struct {
    evio_rx_subscription *subscription;
    int values[16];
    size_t count;
    int error;
    size_t completed;
    size_t captured;
    bool cancel_next;
    bool free_next;
    bool free_terminal;
} rx_log;

static void record_next(void *ctx, const void *value)
{
    rx_log *log = ctx;
    assert_true(log->count < 16);
    log->values[log->count++] = *(const int *)value;
    if (log->free_next) {
        evio_rx_subscription_free(log->subscription);
        log->subscription = NULL;
    } else if (log->cancel_next) {
        evio_rx_subscription_cancel(log->subscription);
    }
}

static void record_error(void *ctx, int error)
{
    rx_log *log = ctx;
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->error = error;
    if (log->free_terminal) {
        evio_rx_subscription_free(log->subscription);
        log->subscription = NULL;
    }
}

static void record_complete(void *ctx)
{
    rx_log *log = ctx;
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->completed++;
    if (log->free_terminal) {
        evio_rx_subscription_free(log->subscription);
        log->subscription = NULL;
    }
}

static evio_rx_observer observer(rx_log *log)
{
    return (evio_rx_observer) {
        .next = record_next,
        .error = record_error,
        .complete = record_complete,
        .ctx = log,
    };
}

static int twice(void *ctx, void *out, const void *value)
{
    *(int *)out = *(const int *)value * 2;
    return 0;
}

static void capture_destroy(void *data)
{
    rx_log *log = data;
    log->captured++;
}

TEST(test_evio_rx_core_independent_snapshot_and_references)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3};
    rx_log first = {0};
    rx_log second = {0};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 3);
    assert_non_null(array);
    evio_rx_observable *ref = evio_rx_observable_ref(array);
    assert_ptr_equal(ref, array);
    evio_rx_observable_free(ref);
    evio_rx_observable *mapped = evio_rx_map(array, &int_type, twice,
                                             (evio_rx_capture) { .data = &first, .destroy = capture_destroy });
    assert_non_null(mapped);
    evio_rx_observable_free(array);
    values[0] = 99;
    first.subscription = evio_rx_subscription_new(loop, mapped, observer(&first), 1);
    second.subscription = evio_rx_subscription_new(loop, mapped, observer(&second), 1);
    assert_non_null(first.subscription);
    assert_non_null(second.subscription);
    evio_rx_observable_free(mapped);
    assert_int_equal(first.count, 0);
    assert_int_equal(first.completed, 0);
    assert_false(evio_rx_subscription_is_closed(first.subscription));

    evio_rx_subscription_start(first.subscription);
    int expected[] = {2, 4, 6};
    assert_int_equal(first.count, 3);
    assert_memory_equal(first.values, expected, sizeof(expected));
    assert_int_equal(first.completed, 1);
    assert_int_equal(first.captured, 0);
    assert_int_equal(second.count, 0);
    evio_rx_subscription_start(first.subscription);
    evio_rx_subscription_cancel(first.subscription);
    evio_rx_subscription_cancel(first.subscription);
    assert_int_equal(first.completed, 1);
    assert_int_equal(first.count, 3);

    evio_rx_subscription_start(second.subscription);
    assert_int_equal(second.count, 3);
    assert_memory_equal(second.values, expected, sizeof(expected));
    assert_int_equal(second.completed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(first.subscription);
    assert_int_equal(first.captured, 0);
    evio_rx_subscription_free(second.subscription);
    assert_int_equal(first.captured, 1);
    evio_loop_free(loop);
}

TEST(test_evio_rx_core_cancel_and_free_from_next)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3};
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 3);
    for (size_t i = 0; i < 2; ++i) {
        rx_log log = { .cancel_next = !i, .free_next = i };
        log.subscription = evio_rx_subscription_new(loop, source, observer(&log), 1);
        assert_non_null(log.subscription);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, 1);
        assert_int_equal(log.values[0], 1);
        assert_int_equal(log.completed, 0);
        assert_int_equal(log.error, 0);
        if (log.subscription) {
            assert_true(evio_rx_subscription_is_closed(log.subscription));
            evio_rx_subscription_start(log.subscription);
            evio_rx_subscription_cancel(log.subscription);
            evio_rx_subscription_free(log.subscription);
        }
    }
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

TEST(test_evio_rx_core_cancel_before_start)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int value = 1;
    rx_log log = {0};
    evio_rx_observable *source = evio_rx_from_array(&int_type, &value, 1);
    evio_rx_observable *mapped = evio_rx_map(source, &int_type, twice,
                                             (evio_rx_capture) { .data = &log, .destroy = capture_destroy });
    evio_rx_observable_free(source);
    log.subscription = evio_rx_subscription_new(loop, mapped, observer(&log), 1);
    assert_non_null(log.subscription);
    evio_rx_observable_free(mapped);
    evio_rx_subscription_cancel(log.subscription);
    assert_true(evio_rx_subscription_is_closed(log.subscription));
    evio_rx_subscription_start(log.subscription);
    evio_rx_subscription_cancel(log.subscription);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, 0);
    assert_int_equal(log.captured, 0);
    evio_rx_subscription_free(log.subscription);
    assert_int_equal(log.captured, 1);
    evio_loop_free(loop);
}

typedef struct {
    evio_rx_subscription *subscription;
    int values[16];
    size_t count;
    size_t completed;
    size_t restarts;
} restart_log;

static void restart_next(void *ctx, const void *value)
{
    restart_log *log = ctx;
    assert_true(log->count < 16);
    log->values[log->count++] = *(const int *)value;
}

static void restart_complete(void *ctx)
{
    restart_log *log = ctx;
    log->completed++;
    if (log->restarts) {
        log->restarts--;
        evio_rx_subscription_restart(log->subscription);
        assert_true(evio_rx_subscription_is_closed(log->subscription));
    }
}

TEST(test_evio_rx_core_restart_repeats_closed_run)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2};
    restart_log log = { .restarts = 1 };
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 2);
    evio_rx_observable *mapped = evio_rx_map(array, &int_type, twice, (evio_rx_capture) {0});
    evio_rx_observable_free(array);
    evio_rx_observer callbacks = { .next = restart_next, .complete = restart_complete, .ctx = &log };
    log.subscription = evio_rx_subscription_new(loop, mapped, callbacks, 1);
    assert_non_null(log.subscription);
    evio_rx_observable_free(mapped);
    evio_rx_subscription_restart(log.subscription);
    int expected[] = {2, 4, 2, 4};
    assert_int_equal(log.count, 4);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(log.completed, 2);
    evio_rx_subscription_cancel(log.subscription);
    evio_rx_subscription_restart(log.subscription);
    assert_int_equal(log.count, 6);
    assert_int_equal(log.completed, 3);
    evio_rx_subscription_free(log.subscription);
    evio_loop_free(loop);
}

TEST(test_evio_rx_core_restart_resubscribes_open_run_once)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    restart_log log = {0};
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observer callbacks = { .next = restart_next, .complete = restart_complete, .ctx = &log };
    log.subscription = evio_rx_subscription_new(loop, source, callbacks, 1);
    assert_non_null(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_subscription_start(log.subscription);
    evio_rx_subscription_restart(log.subscription);
    evio_rx_subscription_restart(log.subscription);
    assert_false(evio_rx_subscription_is_closed(log.subscription));
    int value = 7;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.values[0], 7);
    evio_rx_subject_complete(subject);
    assert_int_equal(log.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

static int fail_on_two(void *ctx, void *out, const void *value)
{
    if (*(const int *)value == 2) {
        return *(int *)ctx;
    }
    return twice(NULL, out, value);
}

TEST(test_evio_rx_core_map_error_and_terminal_free)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3};
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 3);
    int failures[] = {EIO, -EIO};
    for (size_t i = 0; i < 2; ++i) {
        rx_log log = { .free_terminal = true };
        evio_rx_observable *mapped = evio_rx_map(source, &int_type, fail_on_two,
                                                 (evio_rx_capture) { .data = &failures[i] });
        assert_non_null(mapped);
        log.subscription = evio_rx_subscription_new(loop, mapped, observer(&log), 1);
        assert_non_null(log.subscription);
        evio_rx_observable_free(mapped);
        evio_rx_subscription_start(log.subscription);
        assert_null(log.subscription);
        assert_int_equal(log.count, 1);
        assert_int_equal(log.values[0], 2);
        assert_int_equal(log.error, i ? EINVAL : EIO);
        assert_int_equal(log.completed, 0);
    }
    rx_log empty = { .free_terminal = true };
    evio_rx_observable *array = evio_rx_from_array(&int_type, NULL, 0);
    assert_non_null(array);
    empty.subscription = evio_rx_subscription_new(loop, array, observer(&empty), 1);
    evio_rx_subscription_start(empty.subscription);
    assert_null(empty.subscription);
    assert_int_equal(empty.completed, 1);
    assert_int_equal(empty.count, 0);
    evio_rx_observable_free(array);
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

typedef struct {
    alignas(64) int value;
    const void *self;
} aligned_value;

typedef struct {
    size_t attempts;
    size_t copies;
    size_t destroyed;
    size_t live;
    size_t fail_at;
} ownership;

static int aligned_copy(void *ctx, void *out, const void *value)
{
    ownership *owned = ctx;
    const aligned_value *input = value;
    aligned_value *result = out;
    assert_int_equal((uintptr_t)out % 64, 0);
    assert_ptr_equal(input->self, input);
    if (++owned->attempts == owned->fail_at) {
        return EIO;
    }
    result->value = input->value;
    result->self = result;
    owned->copies++;
    owned->live++;
    return 0;
}

static void aligned_destroy(void *ctx, void *value)
{
    ownership *owned = ctx;
    aligned_value *input = value;
    assert_ptr_equal(input->self, input);
    assert_int_equal((uintptr_t)value % 64, 0);
    assert_true(owned->live > 0);
    owned->live--;
    owned->destroyed++;
}

static int aligned_map(void *ctx, void *out, const void *value)
{
    int error = aligned_copy(ctx, out, value);
    if (!error) {
        aligned_value *result = out;
        result->value *= 2;
    }
    return error;
}

static void aligned_next(void *ctx, const void *value)
{
    const aligned_value *input = value;
    assert_ptr_equal(input->self, input);
    assert_int_equal((uintptr_t)value % 64, 0);
    record_next(ctx, &input->value);
}

TEST(test_evio_rx_core_aligned_values_keep_their_addresses)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    ownership owned = {0};
    evio_rx_type type = {
        .size = sizeof(aligned_value), .alignment = alignof(aligned_value),
        .copy = aligned_copy, .destroy = aligned_destroy, .ctx = &owned,
    };
    aligned_value values[3];
    for (size_t i = 0; i < 3; ++i) {
        values[i].value = (int)i + 1;
        values[i].self = &values[i];
    }
    evio_rx_observable *source = evio_rx_from_array(&type, values, 3);
    assert_non_null(source);
    evio_rx_observable *mapped = evio_rx_map(source, &type, aligned_map,
                                             (evio_rx_capture) { .data = &owned });
    assert_non_null(mapped);
    evio_rx_observable_free(source);
    rx_log logs[2] = {0};
    for (size_t i = 0; i < 2; ++i) {
        evio_rx_observer callbacks = observer(&logs[i]);
        callbacks.next = aligned_next;
        logs[i].subscription = evio_rx_subscription_new(loop, mapped, callbacks, 1);
        assert_non_null(logs[i].subscription);
    }
    evio_rx_observable_free(mapped);
    for (size_t i = 0; i < 2; ++i) {
        evio_rx_subscription_start(logs[i].subscription);
        int expected[] = {2, 4, 6};
        assert_int_equal(logs[i].count, 3);
        assert_memory_equal(logs[i].values, expected, sizeof(expected));
        assert_int_equal(logs[i].completed, 1);
        evio_rx_subscription_free(logs[i].subscription);
    }
    assert_int_equal(owned.live, 0);
    assert_int_equal(owned.copies, 9);
    assert_int_equal(owned.destroyed, owned.copies);
    evio_loop_free(loop);
}

TEST(test_evio_rx_core_partial_array_copy_failure)
{
    ownership owned = { .fail_at = 3 };
    evio_rx_type type = {
        .size = sizeof(aligned_value), .alignment = alignof(aligned_value),
        .copy = aligned_copy, .destroy = aligned_destroy, .ctx = &owned,
    };
    aligned_value values[3];
    for (size_t i = 0; i < 3; ++i) {
        values[i].value = (int)i;
        values[i].self = &values[i];
    }
    errno = 0;
    assert_null(evio_rx_from_array(&type, values, 3));
    assert_int_equal(errno, EIO);
    assert_int_equal(owned.attempts, 3);
    assert_int_equal(owned.destroyed, 2);
    assert_int_equal(owned.live, 0);
}

TEST(test_evio_rx_core_invalid_configuration)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int value = 1;
    rx_log log = {0};
    evio_rx_capture capture = { .data = &log, .destroy = capture_destroy };
    evio_rx_observable *source = evio_rx_from_array(&int_type, &value, 1);
    assert_non_null(source);
    evio_rx_type invalid[] = {
        { .size = 0, .alignment = 1 },
        { .size = 4, .alignment = 0 },
        { .size = 6, .alignment = 3 },
        { .size = 4, .alignment = 8 },
        { .size = 4, .alignment = 4, .destroy = aligned_destroy },
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        errno = 0;
        assert_null(evio_rx_from_array(&invalid[i], &value, 1));
        assert_int_equal(errno, EINVAL);
        assert_null(evio_rx_map(source, &invalid[i], twice, capture));
        assert_int_equal(errno, EINVAL);
    }
    assert_null(evio_rx_from_array(NULL, &value, 1));
    assert_null(evio_rx_from_array(&int_type, NULL, 1));
    assert_null(evio_rx_map(NULL, &int_type, twice, capture));
    assert_null(evio_rx_map(source, &int_type, NULL, capture));
    assert_null(evio_rx_map(source, NULL, twice, capture));
    assert_int_equal(log.captured, 0);
    assert_null(evio_rx_subscription_new(NULL, source, observer(&log), 1));
    assert_null(evio_rx_subscription_new(loop, NULL, observer(&log), 1));
    assert_null(evio_rx_subscription_new(loop, source, observer(&log), 0));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_subscription_new(loop, source, observer(&log), SIZE_MAX));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_from_array(&int_type, &value, SIZE_MAX));
    assert_int_equal(errno, EOVERFLOW);
    evio_rx_type huge = { .size = SIZE_MAX / 2 + 1, .alignment = SIZE_MAX / 2 + 1 };
    assert_null(evio_rx_from_array(&huge, &value, 2));
    assert_int_equal(errno, EOVERFLOW);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, 0);
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

typedef struct {
    size_t allocations;
    size_t live;
} allocations;

static void *count_allocations(void *ctx, void *ptr, size_t size)
{
    allocations *counts = ctx;
    if (size) {
        counts->allocations++;
        if (!ptr) {
            counts->live++;
        }
        return realloc(ptr, size);
    }
    if (ptr) {
        counts->live--;
    }
    free(ptr);
    return NULL;
}

static bool greater_than_four(void *ctx, const void *value)
{
    return *(const int *)value > 4;
}

TEST(test_evio_rx_core_no_per_event_allocations)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    void *old_ctx;
    evio_realloc_cb old_allocator = evio_get_allocator(&old_ctx);
    allocations counts = {0};
    evio_set_allocator(count_allocations, &counts);
    int values[] = {1, 2, 3, 4, 5, 6};
    rx_log log = {0};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 6);
    evio_rx_observable *mapped = evio_rx_map(array, &int_type, twice, (evio_rx_capture) {0});
    evio_rx_observable_free(array);
    evio_rx_observable *filtered = evio_rx_filter(mapped, greater_than_four, (evio_rx_capture) {0});
    evio_rx_observable_free(mapped);
    log.subscription = evio_rx_subscription_new(loop, filtered, observer(&log), 1);
    evio_rx_observable_free(filtered);
    size_t prepared = counts.allocations;
    evio_rx_subscription_start(log.subscription);
    evio_rx_subscription_free(log.subscription);
    evio_set_allocator(old_allocator, old_ctx);
    assert_int_equal(counts.allocations, prepared);
    assert_int_equal(counts.live, 0);
    assert_int_equal(log.count, 4);
    assert_int_equal(log.values[3], 12);
    assert_int_equal(log.completed, 1);
    evio_loop_free(loop);
}

TEST(test_evio_rx_core_missing_observer_callbacks_still_close)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3};
    int error = EIO;
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *mapped = evio_rx_map(array, &int_type, fail_on_two,
                                             (evio_rx_capture) { .data = &error });
    evio_rx_subscription *subscription = evio_rx_subscription_new(loop, mapped,
                                                                  (evio_rx_observer) {0}, 1);
    assert_non_null(subscription);
    evio_rx_subscription_start(subscription);
    assert_true(evio_rx_subscription_is_closed(subscription));
    evio_rx_subscription_free(subscription);
    evio_rx_observable_free(mapped);
    evio_rx_observable_free(array);
    evio_loop_free(loop);
}
