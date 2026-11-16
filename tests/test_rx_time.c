#include "test.h"
#include "evio_rx.h"

static const evio_rx_type int_type = {
    .size = sizeof(int), .alignment = alignof(int),
};

typedef struct time_log time_log;
struct time_log {
    evio_loop *loop;
    evio_rx_subscription *subscription;
    evio_rx_subject *subject;
    evio_rx_subject *other;
    int values[16];
    evio_time times[16];
    size_t count;
    size_t completed;
    size_t depth;
    int error;
    void (*action)(time_log *log);
};

static void time_next(void *ctx, const void *value)
{
    time_log *log = ctx;
    assert_int_equal(log->depth++, 0);
    assert_true(log->count < 16);
    log->values[log->count] = *(const int *)value;
    log->times[log->count++] = evio_get_time(log->loop);
    if (log->action) {
        log->action(log);
    }
    log->depth--;
}

static void time_error(void *ctx, int error)
{
    time_log *log = ctx;
    assert_int_equal(log->depth, 0);
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->error = error;
}

static void time_complete(void *ctx)
{
    time_log *log = ctx;
    assert_int_equal(log->depth, 0);
    assert_int_equal(log->completed, 0);
    assert_int_equal(log->error, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->completed++;
}

static evio_rx_observer time_observer(time_log *log)
{
    return (evio_rx_observer) {
        .next = time_next, .error = time_error, .complete = time_complete, .ctx = log,
    };
}

static void advance(evio_loop *loop, evio_time now)
{
    loop->time = now;
    evio_timer_update(loop);
    evio_invoke_pending(loop);
}

TEST(test_evio_rx_debounce_sequence_and_complete_flush)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t flush = 0; flush < 2; ++flush) {
        loop->time = 100;
        evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 2);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_observable *debounced = evio_rx_debounce_time(source, 5);
        time_log log = { .loop = loop };
        log.subscription = evio_rx_subscription_new(loop, debounced, time_observer(&log), 2);
        evio_rx_observable_free(source);
        evio_rx_observable_free(debounced);
        evio_rx_subscription_start(log.subscription);
        int value = 1;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        loop->time = 104;
        value = 2;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        assert_int_equal(log.count, 0);
        advance(loop, 108);
        assert_int_equal(log.count, 0);
        advance(loop, 109);
        assert_int_equal(log.count, 1);
        loop->time = 112;
        value = 3;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        if (flush) {
            loop->time = 113;
            evio_rx_subject_complete(subject);
        } else {
            advance(loop, 116);
            assert_int_equal(log.count, 1);
            advance(loop, 117);
            evio_rx_subject_complete(subject);
        }
        int expected[] = {2, 3};
        assert_int_equal(log.count, 2);
        assert_memory_equal(log.values, expected, sizeof(expected));
        assert_int_equal(log.times[0], 109);
        assert_int_equal(log.times[1], flush ? 113 : 117);
        assert_int_equal(log.completed, 1);
        assert_int_equal(log.error, 0);
        assert_int_equal(loop->refcount, 0);
        advance(loop, 120);
        assert_int_equal(log.count, 2);
        evio_rx_subscription_free(log.subscription);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_debounce_input_on_deadline_flushes_expired_value)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_time arrivals[] = {104, 105, 106};
    evio_time deadlines[] = {109, 110, 111};
    for (size_t i = 0; i < 3; ++i) {
        loop->time = 100;
        evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_observable *debounced = evio_rx_debounce_time(source, 5);
        time_log log = { .loop = loop };
        log.subscription = evio_rx_subscription_new(loop, debounced, time_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        int value = 1;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        loop->time = arrivals[i];
        value = 2;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        assert_int_equal(log.count, i ? 1 : 0);
        if (i) {
            assert_int_equal(log.values[0], 1);
            assert_int_equal(log.times[0], arrivals[i]);
        }
        advance(loop, deadlines[i]);
        assert_int_equal(log.count, i ? 2 : 1);
        assert_int_equal(log.values[log.count - 1], 2);
        assert_int_equal(log.times[log.count - 1], deadlines[i]);
        evio_rx_subject_complete(subject);
        assert_int_equal(log.completed, 1);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(debounced);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_debounce_rearming_clears_native_pending)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *debounced = evio_rx_debounce_time(source, 5);
    time_log log = { .loop = loop };
    log.subscription = evio_rx_subscription_new(loop, debounced, time_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    for (value = 2; value <= 4; ++value) {
        loop->time += 5;
        evio_timer_update(loop);
        assert_int_equal(evio_pending_count(loop), 1);
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        assert_int_equal(log.count, value - 1);
        assert_int_equal(evio_pending_count(loop), 0);
        evio_invoke_pending(loop);
        assert_int_equal(log.count, value - 1);
        assert_int_equal(loop->refcount, 1);
    }
    advance(loop, 120);
    int expected[] = {1, 2, 3, 4};
    assert_int_equal(log.count, 4);
    assert_memory_equal(log.values, expected, sizeof(expected));
    evio_rx_subject_complete(subject);
    assert_int_equal(log.completed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(debounced);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

static void queue_replacement_and_tick(time_log *log)
{
    int value = log->values[log->count - 1];
    if (value == 99) {
        value = 2;
        assert_int_equal(evio_rx_subject_next(log->subject, &value), 0);
        value = 100;
        assert_int_equal(evio_rx_subject_next(log->other, &value), 0);
        evio_invoke_pending(log->loop);
        assert_int_equal(log->count, 1);
    } else if (value == 100) {
        log->loop->time = 110;
    }
}

TEST(test_evio_rx_debounce_old_root_tick_cannot_expire_rearmed_window)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_subject *trigger = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_subject *clock = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *sources[] = {
        evio_rx_debounce_time(source, 5), evio_rx_from_subject(trigger), evio_rx_from_subject(clock),
    };
    evio_rx_observable *merged = evio_rx_merge(sources, 3);
    time_log log = {
        .loop = loop,
        .subject = subject,
        .other = clock,
        .action = queue_replacement_and_tick,
    };
    log.subscription = evio_rx_subscription_new(loop, merged, time_observer(&log), 3);
    evio_rx_subscription_start(log.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    loop->time = 105;
    evio_timer_update(loop);
    assert_int_equal(evio_pending_count(loop), 1);
    value = 99;
    assert_int_equal(evio_rx_subject_next(trigger, &value), 0);
    int expected[] = {99, 1, 100, 2};
    assert_int_equal(log.count, 3);
    assert_memory_equal(log.values, expected, 3 * sizeof(int));
    assert_int_equal(loop->time, 110);
    assert_int_equal(loop->refcount, 1);
    advance(loop, 110);
    assert_int_equal(log.count, 4);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(source);
    for (size_t i = 0; i < 3; ++i) {
        evio_rx_observable_free(sources[i]);
    }
    evio_rx_subject_free(subject);
    evio_rx_subject_free(trigger);
    evio_rx_subject_free(clock);
    evio_loop_free(loop);
}

typedef struct {
    alignas(64) const void *self;
    int *number;
} time_value;

typedef struct {
    time_log log;
    size_t copied;
    size_t destroyed;
    size_t live;
    bool cancel_copy;
    int copy_error;
} value_log;

static int value_copy(void *ctx, void *dst, const void *src)
{
    value_log *log = ctx;
    const time_value *value = src;
    time_value *out = dst;
    assert_ptr_equal(value->self, value);
    assert_int_equal((uintptr_t)out % alignof(time_value), 0);
    if (log->copy_error) {
        return log->copy_error;
    }
    out->self = out;
    out->number = evio_malloc(sizeof(*out->number));
    *out->number = *value->number;
    log->copied++;
    log->live++;
    if (log->cancel_copy) {
        evio_rx_subscription_cancel(log->log.subscription);
        assert_int_equal(*out->number, *value->number);
    }
    return 0;
}

static void value_destroy(void *ctx, void *value)
{
    value_log *log = ctx;
    time_value *number = value;
    assert_ptr_equal(number->self, number);
    assert_true(log->live > 0);
    evio_free(number->number);
    log->live--;
    log->destroyed++;
}

static evio_rx_type value_type(value_log *log)
{
    return (evio_rx_type) {
        .size = sizeof(time_value), .alignment = alignof(time_value),
        .copy = value_copy, .destroy = value_destroy, .ctx = log,
    };
}

static void value_next(void *ctx, const void *value)
{
    value_log *log = ctx;
    const time_value *number = value;
    assert_ptr_equal(number->self, number);
    time_next(&log->log, number->number);
}

static evio_rx_observer value_observer(value_log *log)
{
    return (evio_rx_observer) {
        .next = value_next, .error = time_error, .complete = time_complete, .ctx = log,
    };
}

static void value_next_free(void *ctx, const void *value)
{
    value_log *log = ctx;
    const time_value *number = value;
    assert_ptr_equal(number->self, number);
    assert_int_equal(*number->number, 7);
    log->log.count++;
    evio_rx_subscription_free(log->log.subscription);
    log->log.subscription = NULL;
    assert_ptr_equal(number->self, number);
    assert_int_equal(*number->number, 7);
    assert_int_equal(log->live, 1);
}

TEST(test_evio_rx_debounce_owned_value_survives_free_from_native_callback)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    value_log data = { .log = { .loop = loop } };
    evio_rx_type type = value_type(&data);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *debounced = evio_rx_debounce_time(source, 5);
    data.log.subscription = evio_rx_subscription_new(loop, debounced,
                                                     (evio_rx_observer) { .next = value_next_free, .ctx = &data }, 1);
    evio_rx_observable_free(source);
    evio_rx_observable_free(debounced);
    evio_rx_subscription_start(data.log.subscription);
    int number = 7;
    time_value value = { .self = &value, .number = &number };
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    number = 99;
    advance(loop, 105);
    assert_null(data.log.subscription);
    assert_int_equal(data.log.count, 1);
    assert_int_equal(data.copied, 1);
    assert_int_equal(data.destroyed, 1);
    assert_int_equal(data.live, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_debounce_error_cancel_and_copy_cancel_drop_owned_value)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 3; ++i) {
        loop->time = 100;
        value_log data = { .log = { .loop = loop }, .cancel_copy = i == 2 };
        evio_rx_type type = value_type(&data);
        evio_rx_subject *subject = evio_rx_subject_new(loop, &type, 1);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_observable *debounced = evio_rx_debounce_time(source, 5);
        data.log.subscription = evio_rx_subscription_new(loop, debounced, value_observer(&data), 1);
        evio_rx_subscription_start(data.log.subscription);
        int number = 7;
        time_value value = { .self = &value, .number = &number };
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        if (!i) {
            evio_rx_subject_error(subject, EIO);
        } else {
            evio_rx_subscription_cancel(data.log.subscription);
        }
        advance(loop, 105);
        assert_int_equal(data.log.count, 0);
        assert_int_equal(data.log.completed, 0);
        assert_int_equal(data.log.error, i ? 0 : EIO);
        assert_int_equal(data.copied, 1);
        assert_int_equal(data.destroyed, 1);
        assert_int_equal(data.live, 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(data.log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(debounced);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_debounce_copy_error_drops_previous_value_and_timer)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    value_log data = { .log = { .loop = loop } };
    evio_rx_type type = value_type(&data);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *debounced = evio_rx_debounce_time(source, 5);
    data.log.subscription = evio_rx_subscription_new(loop, debounced, value_observer(&data), 1);
    evio_rx_subscription_start(data.log.subscription);
    int number = 7;
    time_value value = { .self = &value, .number = &number };
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(loop->refcount, 1);
    data.copy_error = ENOSPC;
    loop->time = 104;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(data.log.count, 0);
    assert_int_equal(data.log.completed, 0);
    assert_int_equal(data.log.error, ENOSPC);
    assert_int_equal(data.copied, 1);
    assert_int_equal(data.destroyed, 1);
    assert_int_equal(data.live, 0);
    assert_int_equal(loop->refcount, 0);
    advance(loop, 110);
    assert_int_equal(data.log.count, 0);
    evio_rx_subscription_free(data.log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(debounced);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

static void advance_to_max(time_log *log)
{
    log->loop->time = EVIO_TIME_MAX;
}

TEST(test_evio_rx_debounce_time_overflow_after_callback_and_exact_max_deadline)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 3; ++i) {
        loop->time = i == 2 ? 100 : EVIO_TIME_MAX - 2;
        evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_observable *debounced = evio_rx_debounce_time(source, i == 1 ? 2 : 5);
        time_log log = { .loop = loop, .action = i == 2 ? advance_to_max : NULL };
        log.subscription = evio_rx_subscription_new(loop, debounced, time_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        int value = 1;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        if (i == 1) {
            assert_int_equal(log.error, 0);
            advance(loop, EVIO_TIME_MAX);
            assert_int_equal(log.count, 1);
            evio_rx_subject_complete(subject);
            assert_int_equal(log.completed, 1);
        } else {
            if (i == 2) {
                loop->time = 105;
                value = 2;
                assert_int_equal(evio_rx_subject_next(subject, &value), 0);
                assert_int_equal(log.count, 1);
                assert_int_equal(log.values[0], 1);
            }
            assert_int_equal(log.error, EOVERFLOW);
            assert_int_equal(log.completed, 0);
        }
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(debounced);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_debounce_independent_pending_and_invalid_configuration)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    assert_null(evio_rx_debounce_time(NULL, 5));
    assert_null(evio_rx_debounce_time(source, 0));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable *debounced = evio_rx_debounce_time(source, 5);
    time_log logs[2] = {{ .loop = loop }, { .loop = loop }};
    for (size_t i = 0; i < 2; ++i) {
        logs[i].subscription = evio_rx_subscription_new(loop, debounced, time_observer(&logs[i]), 1);
        evio_rx_subscription_start(logs[i].subscription);
    }
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(loop->refcount, 2);
    loop->time = 105;
    evio_timer_update(loop);
    evio_rx_subscription_cancel(logs[0].subscription);
    evio_invoke_pending(loop);
    assert_int_equal(logs[0].count, 0);
    assert_int_equal(logs[1].count, 1);
    assert_int_equal(logs[1].values[0], 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subject_complete(subject);
    assert_int_equal(logs[0].completed, 0);
    assert_int_equal(logs[1].completed, 1);
    for (size_t i = 0; i < 2; ++i) {
        evio_rx_subscription_free(logs[i].subscription);
    }
    evio_rx_observable_free(source);
    evio_rx_observable_free(debounced);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_throttle_leading_boundary_borrows_values_without_timer)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    value_log data = { .log = { .loop = loop } };
    evio_rx_type type = value_type(&data);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *throttled = evio_rx_throttle_time(source, 5);
    assert_non_null(throttled);
    data.log.subscription = evio_rx_subscription_new(loop, throttled, value_observer(&data), 1);
    evio_rx_subscription_start(data.log.subscription);
    evio_time times[] = {0, 4, 5, 9, 10};
    for (size_t i = 0; i < 5; ++i) {
        loop->time = times[i];
        int number = (int)i + 1;
        time_value value = { .self = &value, .number = &number };
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        assert_int_equal(loop->refcount, 0);
    }
    int expected[] = {1, 3, 5};
    evio_time expected_times[] = {0, 5, 10};
    assert_int_equal(data.log.count, 3);
    assert_memory_equal(data.log.values, expected, sizeof(expected));
    assert_memory_equal(data.log.times, expected_times, sizeof(expected_times));
    assert_int_equal(data.copied, 0);
    assert_int_equal(data.destroyed, 0);
    assert_int_equal(data.live, 0);
    evio_rx_subject_complete(subject);
    assert_int_equal(data.log.completed, 1);
    assert_int_equal(data.log.error, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(data.log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(throttled);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_throttle_complete_and_error_do_not_wait_for_window)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        loop->time = 100;
        evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_observable *throttled = evio_rx_throttle_time(source, 5);
        time_log log = { .loop = loop };
        log.subscription = evio_rx_subscription_new(loop, throttled, time_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        int value = 1;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        value = 2;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        if (i) {
            evio_rx_subject_error(subject, EIO);
        } else {
            evio_rx_subject_complete(subject);
        }
        assert_int_equal(log.count, 1);
        assert_int_equal(log.values[0], 1);
        assert_int_equal(log.completed, i ? 0 : 1);
        assert_int_equal(log.error, i ? EIO : 0);
        assert_int_equal(loop->time, 100);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(throttled);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

static void cancel_next(time_log *log)
{
    evio_rx_subscription_cancel(log->subscription);
}

TEST(test_evio_rx_throttle_synchronous_burst_and_cancel_first)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    int values[] = {1, 2, 3};
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *throttled = evio_rx_throttle_time(source, 5);
    for (size_t i = 0; i < 2; ++i) {
        time_log log = { .loop = loop, .action = i ? cancel_next : NULL };
        log.subscription = evio_rx_subscription_new(loop, throttled, time_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, 1);
        assert_int_equal(log.values[0], 1);
        assert_int_equal(log.completed, i ? 0 : 1);
        assert_int_equal(log.error, 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
    }
    evio_rx_observable_free(source);
    evio_rx_observable_free(throttled);
    evio_loop_free(loop);
}

TEST(test_evio_rx_throttle_overflow_before_first_and_next_window)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_time bases[] = {EVIO_TIME_MAX - 2, 100, EVIO_TIME_MAX - 2};
    evio_time durations[] = {3, 5, 2};
    for (size_t i = 0; i < 3; ++i) {
        loop->time = bases[i];
        evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_observable *throttled = evio_rx_throttle_time(source, durations[i]);
        time_log log = { .loop = loop };
        log.subscription = evio_rx_subscription_new(loop, throttled, time_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        int value = 1;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        if (i) {
            assert_int_equal(log.error, 0);
            assert_int_equal(log.count, 1);
            loop->time = EVIO_TIME_MAX;
            value = 2;
            assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        }
        assert_int_equal(log.count, i ? 1 : 0);
        assert_int_equal(log.error, EOVERFLOW);
        assert_int_equal(log.completed, 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(throttled);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_throttle_independent_windows_and_invalid_configuration)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    assert_null(evio_rx_throttle_time(NULL, 5));
    assert_null(evio_rx_throttle_time(source, 0));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable *throttled = evio_rx_throttle_time(source, 5);
    time_log first = { .loop = loop };
    time_log second = { .loop = loop };
    first.subscription = evio_rx_subscription_new(loop, throttled, time_observer(&first), 1);
    second.subscription = evio_rx_subscription_new(loop, throttled, time_observer(&second), 1);
    evio_rx_subscription_start(first.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    loop->time = 104;
    evio_rx_subscription_start(second.subscription);
    value = 2;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    loop->time = 105;
    value = 3;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    int expected[] = {1, 3};
    assert_int_equal(first.count, 2);
    assert_memory_equal(first.values, expected, sizeof(expected));
    assert_int_equal(second.count, 1);
    assert_int_equal(second.values[0], 2);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subject_complete(subject);
    assert_int_equal(first.completed, 1);
    assert_int_equal(second.completed, 1);
    evio_rx_subscription_free(first.subscription);
    evio_rx_subscription_free(second.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(throttled);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_delay_deadlines_and_deferred_complete)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *delayed = evio_rx_delay(source, 5, 2);
    assert_non_null(delayed);
    time_log log = { .loop = loop };
    log.subscription = evio_rx_subscription_new(loop, delayed, time_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    loop->time = 102;
    value = 2;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    value = 99;
    loop->time = 103;
    evio_rx_subject_complete(subject);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(loop->refcount, 1);
    advance(loop, 104);
    assert_int_equal(log.count, 0);
    advance(loop, 105);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.completed, 0);
    assert_int_equal(loop->refcount, 1);
    advance(loop, 106);
    assert_int_equal(log.count, 1);
    advance(loop, 107);
    int expected[] = {1, 2};
    evio_time expected_times[] = {105, 107};
    assert_int_equal(log.count, 2);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_memory_equal(log.times, expected_times, sizeof(expected_times));
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(delayed);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_throttle_and_delay_restart_drop_previous_run_state)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    int values[] = {1, 2};
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 2);
    evio_rx_observable *throttled = evio_rx_throttle_time(array, 5);
    time_log log = { .loop = loop };
    log.subscription = evio_rx_subscription_new(loop, throttled, time_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    log = (time_log) { .loop = loop, .subscription = log.subscription };
    evio_rx_subscription_restart(log.subscription);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.values[0], 1);
    assert_int_equal(log.completed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(throttled);

    evio_rx_observable *delayed = evio_rx_delay(array, 5, 2);
    log = (time_log) { .loop = loop };
    log.subscription = evio_rx_subscription_new(loop, delayed, time_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    loop->time = 101;
    evio_rx_subscription_restart(log.subscription);
    advance(loop, 105);
    assert_int_equal(log.count, 0);
    advance(loop, 106);
    advance(loop, 106);
    evio_time expected_times[] = {106, 106};
    assert_int_equal(log.count, 2);
    assert_memory_equal(log.values, values, sizeof(values));
    assert_memory_equal(log.times, expected_times, sizeof(expected_times));
    assert_int_equal(log.completed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(delayed);
    evio_rx_observable_free(array);
    evio_loop_free(loop);
}

TEST(test_evio_rx_delay_equal_deadline_fifo_independent_runs_and_zero_async)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3};
    evio_time durations[] = {0, 5};
    for (size_t i = 0; i < 2; ++i) {
        loop->time = 100;
        evio_rx_observable *source = evio_rx_from_array(&int_type, values, 3);
        evio_rx_observable *delayed = evio_rx_delay(source, durations[i], 3);
        time_log logs[2] = {{ .loop = loop }, { .loop = loop }};
        for (size_t j = 0; j < 2; ++j) {
            logs[j].subscription = evio_rx_subscription_new(loop, delayed, time_observer(&logs[j]), 1);
        }
        evio_rx_observable_free(source);
        evio_rx_observable_free(delayed);
        for (size_t j = 0; j < 2; ++j) {
            evio_rx_subscription_start(logs[j].subscription);
            assert_int_equal(logs[j].count, 0);
            assert_int_equal(logs[j].completed, 0);
        }
        assert_int_equal(loop->refcount, 2);
        evio_invoke_pending(loop);
        assert_int_equal(logs[1].count, 0);
        loop->time = i ? 105 : 100;
        evio_timer_update(loop);
        assert_int_equal(evio_pending_count(loop), 2);
        evio_rx_subscription_cancel(logs[0].subscription);
        evio_invoke_pending(loop);
        advance(loop, i ? 105 : 100);
        advance(loop, i ? 105 : 100);
        assert_int_equal(logs[0].count, 0);
        assert_int_equal(logs[0].completed, 0);
        assert_int_equal(logs[1].count, 3);
        assert_memory_equal(logs[1].values, values, sizeof(values));
        assert_int_equal(logs[1].times[0], i ? 105 : 100);
        assert_int_equal(logs[1].times[1], logs[1].times[0]);
        assert_int_equal(logs[1].times[2], logs[1].times[0]);
        assert_int_equal(logs[1].completed, 1);
        assert_int_equal(loop->refcount, 0);
        for (size_t j = 0; j < 2; ++j) {
            evio_rx_subscription_free(logs[j].subscription);
        }
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_delay_error_cancel_and_copy_cancel_destroy_owned_queue)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 3; ++i) {
        loop->time = 100;
        value_log data = { .log = { .loop = loop }, .cancel_copy = i == 2 };
        evio_rx_type type = value_type(&data);
        evio_rx_subject *subject = evio_rx_subject_new(loop, &type, 1);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_observable *delayed = evio_rx_delay(source, 5, 2);
        data.log.subscription = evio_rx_subscription_new(loop, delayed, value_observer(&data), 1);
        evio_rx_subscription_start(data.log.subscription);
        int number = 7;
        time_value value = { .self = &value, .number = &number };
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        if (i != 2) {
            loop->time = 102;
            number = 8;
            assert_int_equal(evio_rx_subject_next(subject, &value), 0);
            assert_int_equal(loop->refcount, 1);
        }
        loop->time = 103;
        if (!i) {
            evio_rx_subject_error(subject, EIO);
        } else {
            evio_rx_subscription_cancel(data.log.subscription);
        }
        advance(loop, 110);
        assert_int_equal(data.log.count, 0);
        assert_int_equal(data.log.completed, 0);
        assert_int_equal(data.log.error, i ? 0 : EIO);
        assert_int_equal(data.copied, i == 2 ? 1 : 2);
        assert_int_equal(data.destroyed, data.copied);
        assert_int_equal(data.live, 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(data.log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(delayed);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_delay_copy_error_clears_partial_slot_and_pending_head)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    value_log data = { .log = { .loop = loop } };
    evio_rx_type type = value_type(&data);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *delayed = evio_rx_delay(source, 5, 2);
    data.log.subscription = evio_rx_subscription_new(loop, delayed, value_observer(&data), 1);
    evio_rx_subscription_start(data.log.subscription);
    int number = 7;
    time_value value = { .self = &value, .number = &number };
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    data.copy_error = ENOSPC;
    number = 8;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(data.log.error, ENOSPC);
    assert_int_equal(data.log.count, 0);
    assert_int_equal(data.copied, 1);
    assert_int_equal(data.destroyed, 1);
    assert_int_equal(data.live, 0);
    assert_int_equal(loop->refcount, 0);
    advance(loop, 110);
    assert_int_equal(data.log.count, 0);
    evio_rx_subscription_free(data.log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(delayed);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

static void delay_value_next_free(void *ctx, const void *value)
{
    value_log *log = ctx;
    const time_value *number = value;
    assert_ptr_equal(number->self, number);
    assert_int_equal(*number->number, 7);
    assert_int_equal(log->live, 2);
    log->log.count++;
    evio_rx_subscription_free(log->log.subscription);
    log->log.subscription = NULL;
    assert_ptr_equal(number->self, number);
    assert_int_equal(*number->number, 7);
    assert_int_equal(log->live, 2);
}

TEST(test_evio_rx_delay_free_from_next_keeps_value_and_discards_tail)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    value_log data = { .log = { .loop = loop } };
    evio_rx_type type = value_type(&data);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *delayed = evio_rx_delay(source, 5, 2);
    data.log.subscription = evio_rx_subscription_new(loop, delayed,
                                                     (evio_rx_observer) { .next = delay_value_next_free, .ctx = &data }, 1);
    evio_rx_subscription_start(data.log.subscription);
    int number = 7;
    time_value value = { .self = &value, .number = &number };
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    number = 8;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    advance(loop, 105);
    assert_null(data.log.subscription);
    assert_int_equal(data.log.count, 1);
    assert_int_equal(data.copied, 2);
    assert_int_equal(data.destroyed, 2);
    assert_int_equal(data.live, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_observable_free(source);
    evio_rx_observable_free(delayed);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

static void error_on_next(time_log *log)
{
    evio_rx_subject_error(log->subject, EIO);
}

TEST(test_evio_rx_delay_reentrant_upstream_error_discards_already_due_tail)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    value_log data = { .log = { .loop = loop, .action = error_on_next } };
    evio_rx_type type = value_type(&data);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &type, 1);
    data.log.subject = subject;
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *delayed = evio_rx_delay(source, 5, 2);
    data.log.subscription = evio_rx_subscription_new(loop, delayed, value_observer(&data), 1);
    evio_rx_subscription_start(data.log.subscription);
    int number = 7;
    time_value value = { .self = &value, .number = &number };
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    number = 8;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    advance(loop, 105);
    assert_int_equal(data.log.count, 1);
    assert_int_equal(data.log.values[0], 7);
    assert_int_equal(data.log.error, EIO);
    assert_int_equal(data.log.completed, 0);
    assert_int_equal(data.copied, 2);
    assert_int_equal(data.destroyed, 2);
    assert_int_equal(data.live, 0);
    assert_int_equal(loop->refcount, 0);
    advance(loop, 110);
    assert_int_equal(data.log.count, 1);
    evio_rx_subscription_free(data.log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(delayed);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_delay_upstream_error_precedes_ready_native_timer)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *delayed = evio_rx_delay(source, 5, 1);
    time_log log = { .loop = loop };
    log.subscription = evio_rx_subscription_new(loop, delayed, time_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    loop->time = 105;
    evio_timer_update(loop);
    assert_int_equal(evio_pending_count(loop), 1);
    evio_rx_subject_error(subject, EIO);
    assert_int_equal(log.error, EIO);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(evio_pending_count(loop), 0);
    evio_invoke_pending(loop);
    assert_int_equal(log.count, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(delayed);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

static int count_delayed_input(void *ctx, void *out, const void *value)
{
    size_t *calls = ctx;
    (*calls)++;
    *(int *)out = *(const int *)value;
    return 0;
}

TEST(test_evio_rx_delay_capacity_overflow_stops_synchronous_producer)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    int values[] = {1, 2, 3};
    size_t calls = 0;
    evio_rx_observable *array = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *source = evio_rx_map(array, &int_type, count_delayed_input,
                                             (evio_rx_capture) { .data = &calls });
    evio_rx_observable *delayed = evio_rx_delay(source, 5, 1);
    time_log log = { .loop = loop };
    log.subscription = evio_rx_subscription_new(loop, delayed, time_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(calls, 2);
    assert_int_equal(log.error, EOVERFLOW);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(loop->refcount, 0);
    advance(loop, 110);
    assert_int_equal(log.count, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(array);
    evio_rx_observable_free(source);
    evio_rx_observable_free(delayed);
    evio_loop_free(loop);
}

static void overflow_root(time_log *log)
{
    int value = 2;
    assert_int_equal(evio_rx_subject_next(log->subject, &value), 0);
    value = 3;
    assert_int_equal(evio_rx_subject_next(log->subject, &value), 0);
    assert_int_equal(log->count, 1);
    assert_int_equal(log->error, 0);
}

TEST(test_evio_rx_delay_root_overflow_is_not_delay_capacity_or_subject_error)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *delayed = evio_rx_delay(source, 5, 4);
    time_log log = { .loop = loop, .subject = subject, .action = overflow_root };
    time_log other = { .loop = loop };
    log.subscription = evio_rx_subscription_new(loop, delayed, time_observer(&log), 1);
    other.subscription = evio_rx_subscription_new(loop, source, time_observer(&other), 1);
    evio_rx_subscription_start(log.subscription);
    evio_rx_subscription_start(other.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    advance(loop, 105);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.values[0], 1);
    assert_int_equal(log.error, EOVERFLOW);
    assert_int_equal(log.completed, 0);
    int expected[] = {1, 2, 3};
    assert_int_equal(other.count, 3);
    assert_memory_equal(other.values, expected, sizeof(expected));
    assert_int_equal(other.error, 0);
    value = 4;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(other.count, 4);
    assert_int_equal(other.values[3], 4);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_subscription_free(other.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(delayed);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_delay_overflow_and_max_deadline_including_zero_duration)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_time bases[] = {EVIO_TIME_MAX - 2, EVIO_TIME_MAX - 2, EVIO_TIME_MAX};
    evio_time durations[] = {5, 2, 0};
    for (size_t i = 0; i < 3; ++i) {
        loop->time = bases[i];
        evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_observable *delayed = evio_rx_delay(source, durations[i], 1);
        time_log log = { .loop = loop };
        log.subscription = evio_rx_subscription_new(loop, delayed, time_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        int value = 1;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        assert_int_equal(log.count, 0);
        if (i) {
            evio_rx_subject_complete(subject);
            assert_int_equal(log.completed, 0);
            advance(loop, EVIO_TIME_MAX);
            assert_int_equal(log.count, 1);
            assert_int_equal(log.values[0], 1);
            assert_int_equal(log.times[0], EVIO_TIME_MAX);
            assert_int_equal(log.completed, 1);
            assert_int_equal(log.error, 0);
        } else {
            assert_int_equal(log.error, EOVERFLOW);
            assert_int_equal(log.completed, 0);
        }
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(delayed);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_delay_callback_clock_advance_drains_already_due_tail)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *delayed = evio_rx_delay(source, 5, 2);
    time_log log = { .loop = loop, .action = advance_to_max };
    log.subscription = evio_rx_subscription_new(loop, delayed, time_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    loop->time = 102;
    value = 2;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    evio_rx_subject_complete(subject);
    advance(loop, 105);
    advance(loop, EVIO_TIME_MAX);
    int expected[] = {1, 2};
    assert_int_equal(log.count, 2);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(log.times[0], 105);
    assert_int_equal(log.times[1], EVIO_TIME_MAX);
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(delayed);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_delay_empty_source_and_invalid_configuration)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_observable *source = evio_rx_from_array(&int_type, NULL, 0);
    assert_null(evio_rx_delay(NULL, 0, 1));
    assert_null(evio_rx_delay(source, 0, 0));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_delay(source, 0, SIZE_MAX));
    assert_int_equal(errno, EOVERFLOW);
    evio_rx_observable *delayed = evio_rx_delay(source, 0, 1);
    time_log log = { .loop = loop };
    log.subscription = evio_rx_subscription_new(loop, delayed, time_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(delayed);
    evio_loop_free(loop);
}

TEST(test_evio_rx_timeout_resets_after_each_value)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *timed = evio_rx_timeout(source, 5);
    assert_non_null(timed);
    time_log log = { .loop = loop };
    log.subscription = evio_rx_subscription_new(loop, timed, time_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(loop->refcount, 1);
    advance(loop, 104);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    advance(loop, 108);
    value = 2;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    advance(loop, 112);
    int expected[] = {1, 2};
    evio_time expected_times[] = {104, 108};
    assert_int_equal(log.count, 2);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_memory_equal(log.times, expected_times, sizeof(expected_times));
    assert_int_equal(log.error, 0);
    advance(loop, 113);
    assert_int_equal(log.error, ETIMEDOUT);
    assert_int_equal(log.completed, 0);
    assert_int_equal(loop->refcount, 0);
    value = 3;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(log.count, 2);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(timed);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_timeout_deadline_equality_in_both_callback_orders_and_late_input)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_time arrivals[] = {105, 105, 106};
    for (size_t i = 0; i < 3; ++i) {
        loop->time = 100;
        evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_observable *timed = evio_rx_timeout(source, 5);
        time_log log = { .loop = loop };
        log.subscription = evio_rx_subscription_new(loop, timed, time_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        loop->time = arrivals[i];
        evio_timer_update(loop);
        assert_int_equal(evio_pending_count(loop), 1);
        if (i == 1) {
            evio_invoke_pending(loop);
        }
        int value = 1;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        evio_invoke_pending(loop);
        assert_int_equal(log.count, 0);
        assert_int_equal(log.completed, 0);
        assert_int_equal(log.error, ETIMEDOUT);
        assert_int_equal(evio_pending_count(loop), 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(timed);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

static int failing_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    return EACCES;
}

static void unused_start(evio_rx_emitter *emitter, void *state)
{
    fail();
}

static const evio_rx_source failing_source = {
    .prepare = failing_prepare, .start = unused_start,
};

TEST(test_evio_rx_time_operators_source_prepare_failure_fails_new)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_observable *source = evio_rx_observable_new(&int_type, failing_source,
                                                        (evio_rx_capture) {0});
    evio_rx_observable *operators[] = {
        evio_rx_debounce_time(source, 5),
        evio_rx_throttle_time(source, 5),
        evio_rx_delay(source, 5, 2),
        evio_rx_timeout(source, 5),
    };
    for (size_t i = 0; i < 4; ++i) {
        time_log log = { .loop = loop };
        assert_null(evio_rx_subscription_new(loop, operators[i], time_observer(&log), 1));
        assert_int_equal(errno, EACCES);
        assert_int_equal(loop->refcount, 0);
        evio_rx_observable_free(operators[i]);
    }
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

TEST(test_evio_rx_timeout_late_complete_before_tick_times_out)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *timed = evio_rx_timeout(source, 5);
    time_log log = { .loop = loop };
    log.subscription = evio_rx_subscription_new(loop, timed, time_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    loop->time = 105;
    evio_timer_update(loop);
    evio_rx_subject_complete(subject);
    evio_invoke_pending(loop);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, ETIMEDOUT);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(timed);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

static void old_timeout_pending(time_log *log)
{
    log->loop->time = 105;
    evio_timer_update(log->loop);
    assert_int_equal(evio_pending_count(log->loop), 1);
}

static void old_timeout_queued(time_log *log)
{
    old_timeout_pending(log);
    evio_invoke_pending(log->loop);
    assert_int_equal(log->count, 1);
    assert_int_equal(log->error, 0);
}

TEST(test_evio_rx_timeout_rearm_invalidates_native_pending_and_old_root_tick)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        loop->time = 100;
        evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_observable *timed = evio_rx_timeout(source, 5);
        time_log log = { .loop = loop, .action = i ? old_timeout_queued : old_timeout_pending };
        log.subscription = evio_rx_subscription_new(loop, timed, time_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        loop->time = 104;
        int value = 1;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        assert_int_equal(log.count, 1);
        assert_int_equal(log.values[0], 1);
        assert_int_equal(log.error, 0);
        assert_int_equal(evio_pending_count(loop), 0);
        assert_int_equal(loop->refcount, 1);
        advance(loop, 109);
        assert_int_equal(log.error, 0);
        advance(loop, 110);
        assert_int_equal(log.error, ETIMEDOUT);
        assert_int_equal(log.count, 1);
        assert_int_equal(log.completed, 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(timed);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

static void free_next(time_log *log)
{
    evio_rx_subscription_free(log->subscription);
    log->subscription = NULL;
}

TEST(test_evio_rx_timeout_borrows_values_and_stops_on_terminal_cancel_or_free)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 4; ++i) {
        loop->time = 100;
        value_log data = { .log = { .loop = loop } };
        if (i == 1) {
            data.log.action = cancel_next;
        } else if (i == 3) {
            data.log.action = free_next;
        }
        evio_rx_type type = value_type(&data);
        evio_rx_subject *subject = evio_rx_subject_new(loop, &type, 1);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        evio_rx_observable *timed = evio_rx_timeout(source, 5);
        data.log.subscription = evio_rx_subscription_new(loop, timed, value_observer(&data), 1);
        evio_rx_subscription_start(data.log.subscription);
        int number = 7;
        time_value value = { .self = &value, .number = &number };
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        loop->time = 104;
        if (i == 2) {
            evio_rx_subject_error(subject, EIO);
        } else {
            evio_rx_subject_complete(subject);
        }
        advance(loop, 110);
        assert_int_equal(data.log.count, 1);
        assert_int_equal(data.log.values[0], 7);
        assert_int_equal(data.log.completed, i ? 0 : 1);
        assert_int_equal(data.log.error, i == 2 ? EIO : 0);
        assert_int_equal(data.copied, 0);
        assert_int_equal(data.destroyed, 0);
        assert_int_equal(data.live, 0);
        assert_int_equal(loop->refcount, 0);
        if (i == 3) {
            assert_null(data.log.subscription);
        } else {
            assert_true(evio_rx_subscription_is_closed(data.log.subscription));
            evio_rx_subscription_free(data.log.subscription);
        }
        evio_rx_observable_free(source);
        evio_rx_observable_free(timed);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

static void timeout_error_free(void *ctx, int error)
{
    time_log *log = ctx;
    time_error(log, error);
    evio_rx_subscription_free(log->subscription);
    log->subscription = NULL;
}

TEST(test_evio_rx_timeout_no_first_value_and_free_from_native_error)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    evio_rx_observable *timed = evio_rx_timeout(source, 5);
    time_log log = { .loop = loop };
    evio_rx_observer observer = time_observer(&log);
    observer.error = timeout_error_free;
    log.subscription = evio_rx_subscription_new(loop, timed, observer, 1);
    evio_rx_subscription_start(log.subscription);
    advance(loop, 104);
    assert_int_equal(log.error, 0);
    advance(loop, 105);
    assert_null(log.subscription);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, ETIMEDOUT);
    assert_int_equal(loop->refcount, 0);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    advance(loop, 110);
    assert_int_equal(log.count, 0);
    evio_rx_observable_free(source);
    evio_rx_observable_free(timed);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

typedef enum {
    TIMEOUT_SILENT,
    TIMEOUT_COMPLETE,
    TIMEOUT_ERROR,
    TIMEOUT_VALUE,
} timeout_mode;

typedef struct {
    evio_loop *loop;
    timeout_mode mode;
    size_t prepared;
    size_t started;
    size_t stopped;
    size_t destroyed;
    size_t refs_at_start;
} timeout_probe;

static int timeout_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    timeout_probe *probe = ctx;
    assert_ptr_equal(loop, probe->loop);
    probe->prepared++;
    *state = probe;
    return 0;
}

static void timeout_source_start(evio_rx_emitter *emitter, void *state)
{
    timeout_probe *probe = state;
    probe->started++;
    probe->refs_at_start = probe->loop->refcount;
    switch (probe->mode) {
        case TIMEOUT_SILENT:
            break;
        case TIMEOUT_COMPLETE:
            evio_rx_emitter_complete(emitter);
            break;
        case TIMEOUT_ERROR:
            evio_rx_emitter_error(emitter, EIO);
            break;
        case TIMEOUT_VALUE: {
            int value = 1;
            if (evio_rx_emitter_next(emitter, &value)) {
                evio_rx_emitter_complete(emitter);
            }
            break;
        }
    }
}

static void timeout_source_stop(void *state)
{
    timeout_probe *probe = state;
    probe->stopped++;
}

static void timeout_source_destroy(void *state)
{
    timeout_probe *probe = state;
    probe->destroyed++;
}

static const evio_rx_source timeout_source = {
    .prepare = timeout_prepare, .start = timeout_source_start,
    .stop = timeout_source_stop, .destroy = timeout_source_destroy,
};

TEST(test_evio_rx_timeout_timer_precedes_source_start_and_sync_terminal)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        loop->time = 100;
        timeout_probe probe = { .loop = loop, .mode = i ? TIMEOUT_ERROR : TIMEOUT_COMPLETE };
        evio_rx_observable *source = evio_rx_observable_new(&int_type, timeout_source,
                                                            (evio_rx_capture) { .data = &probe });
        evio_rx_observable *timed = evio_rx_timeout(source, 5);
        time_log log = { .loop = loop };
        log.subscription = evio_rx_subscription_new(loop, timed, time_observer(&log), 1);
        assert_int_equal(probe.prepared, 1);
        assert_int_equal(probe.started, 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(probe.refs_at_start, 1);
        assert_int_equal(probe.started, 1);
        assert_int_equal(probe.stopped, 1);
        assert_int_equal(probe.destroyed, 1);
        assert_int_equal(log.count, 0);
        assert_int_equal(log.completed, i ? 0 : 1);
        assert_int_equal(log.error, i ? EIO : 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(timed);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_timeout_overflow_before_source_start_and_after_next_callback)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_time bases[] = {EVIO_TIME_MAX - 2, EVIO_TIME_MAX - 2, 100};
    evio_time durations[] = {3, 2, 5};
    timeout_mode modes[] = {TIMEOUT_COMPLETE, TIMEOUT_SILENT, TIMEOUT_VALUE};
    for (size_t i = 0; i < 3; ++i) {
        loop->time = bases[i];
        timeout_probe probe = { .loop = loop, .mode = modes[i] };
        evio_rx_observable *source = evio_rx_observable_new(&int_type, timeout_source,
                                                            (evio_rx_capture) { .data = &probe });
        evio_rx_observable *timed = evio_rx_timeout(source, durations[i]);
        time_log log = { .loop = loop, .action = i == 2 ? advance_to_max : NULL };
        log.subscription = evio_rx_subscription_new(loop, timed, time_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        if (i == 1) {
            assert_int_equal(log.error, 0);
            advance(loop, EVIO_TIME_MAX);
        }
        assert_int_equal(probe.prepared, 1);
        assert_int_equal(probe.started, i ? 1 : 0);
        assert_int_equal(probe.stopped, i ? 1 : 0);
        assert_int_equal(probe.destroyed, 1);
        assert_int_equal(log.error, i == 1 ? ETIMEDOUT : EOVERFLOW);
        assert_int_equal(log.completed, 0);
        assert_int_equal(log.count, i == 2 ? 1 : 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(timed);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_timeout_synchronous_array_and_invalid_configuration)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    int values[] = {1, 2, 3};
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 3);
    assert_null(evio_rx_timeout(NULL, 5));
    assert_null(evio_rx_timeout(source, 0));
    assert_int_equal(errno, EINVAL);
    evio_rx_observable *timed = evio_rx_timeout(source, 5);
    time_log log = { .loop = loop };
    log.subscription = evio_rx_subscription_new(loop, timed, time_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.count, 3);
    assert_memory_equal(log.values, values, sizeof(values));
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(loop->refcount, 0);
    assert_int_equal(evio_pending_count(loop), 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(timed);
    evio_loop_free(loop);
}
