#include "test.h"
#include "evio_rx.h"

static const evio_rx_type int_type = {
    .size = sizeof(int), .alignment = alignof(int),
};

typedef struct subject_log subject_log;
struct subject_log {
    evio_rx_subject *subject;
    evio_loop *loop;
    evio_rx_observable *source;
    evio_rx_subscription *subscription;
    subject_log *other;
    subject_log *added;
    int values[8];
    size_t count;
    size_t completed;
    int error;
    size_t depth;
    int terminal;
    void (*action)(subject_log *log);
};

static void subject_next(void *ctx, const void *value)
{
    subject_log *log = ctx;
    assert_int_equal(log->depth++, 0);
    assert_true(log->count < 8);
    log->values[log->count++] = *(const int *)value;
    if (log->action) {
        log->action(log);
    }
    log->depth--;
}

static void subject_error(void *ctx, int error)
{
    subject_log *log = ctx;
    assert_int_equal(log->depth, 0);
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->error = error;
}

static void subject_complete(void *ctx)
{
    subject_log *log = ctx;
    assert_int_equal(log->depth, 0);
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->completed++;
}

static evio_rx_observer observer(subject_log *log)
{
    return (evio_rx_observer) {
        .next = subject_next, .error = subject_error, .complete = subject_complete, .ctx = log,
    };
}

static void publish_nested(subject_log *log)
{
    if (log->count != 1) {
        return;
    }
    assert_int_equal(log->other->count, 0);
    int value = 2;
    assert_int_equal(evio_rx_subject_next(log->subject, &value), 0);
    value = 99;
    assert_int_equal(log->count, 1);
    if (log->terminal > 0) {
        evio_rx_subject_error(log->subject, log->terminal);
    } else if (log->terminal < 0) {
        evio_rx_subject_complete(log->subject);
    }
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
}

TEST(test_evio_rx_subject_nested_publication_and_terminal_order)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int terminals[] = {0, -1, EIO};
    for (size_t i = 0; i < 3; ++i) {
        evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 2);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        subject_log second = {0};
        subject_log first = {
            .subject = subject,
            .other = &second,
            .action = publish_nested,
            .terminal = terminals[i],
        };
        first.subscription = evio_rx_subscription_new(loop, source, observer(&first), 2);
        second.subscription = evio_rx_subscription_new(loop, source, observer(&second), 2);
        assert_non_null(first.subscription);
        assert_non_null(second.subscription);
        evio_rx_subscription_start(first.subscription);
        evio_rx_subscription_start(second.subscription);
        int value = 1;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        int expected[] = {1, 2};
        for (size_t j = 0; j < 2; ++j) {
            subject_log *log = j ? &second : &first;
            assert_int_equal(log->count, i == 2 ? 1 : 2);
            assert_memory_equal(log->values, expected, log->count * sizeof(int));
            assert_int_equal(log->completed, i == 1);
            assert_int_equal(log->error, i == 2 ? EIO : 0);
            evio_rx_subscription_free(log->subscription);
        }
        evio_rx_observable_free(source);
        evio_rx_subject_free(subject);
    }
    assert_int_equal(loop->refcount, 0);
    evio_loop_free(loop);
}

static void add_and_remove(subject_log *log)
{
    if (log->count != 1) {
        return;
    }
    log->added->subscription = evio_rx_subscription_new(log->loop, log->source,
                                                        observer(log->added), 2);
    assert_non_null(log->added->subscription);
    evio_rx_subscription_start(log->added->subscription);
    evio_rx_subscription_free(log->other->subscription);
    log->other->subscription = NULL;
}

static void add_between_nested(subject_log *log)
{
    if (log->count != 1) {
        return;
    }
    int values[] = {2, 3};
    assert_int_equal(evio_rx_subject_next(log->subject, &values[0]), 0);
    log->added->subscription = evio_rx_subscription_new(log->loop, log->source,
                                                        observer(log->added), 2);
    evio_rx_subscription_start(log->added->subscription);
    assert_int_equal(evio_rx_subject_next(log->subject, &values[1]), 0);
}

TEST(test_evio_rx_subject_late_subscriber_skips_values_queued_before_it)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    subject_log added = {0};
    subject_log first = {
        .subject = subject, .loop = loop, .source = source, .added = &added,
        .action = add_between_nested,
    };
    first.subscription = evio_rx_subscription_new(loop, source, observer(&first), 2);
    evio_rx_subscription_start(first.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    int expected[] = {1, 2, 3};
    assert_int_equal(first.count, 3);
    assert_memory_equal(first.values, expected, sizeof(expected));
    assert_int_equal(added.count, 1);
    assert_int_equal(added.values[0], 3);
    value = 4;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(added.count, 2);
    assert_int_equal(added.values[1], 4);
    evio_rx_subscription_free(added.subscription);
    evio_rx_subscription_free(first.subscription);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_subject_membership_snapshot_and_cancelled_successor)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    subject_log removed = {0};
    subject_log added = {0};
    subject_log first = {
        .loop = loop, .source = source, .other = &removed, .added = &added, .action = add_and_remove,
    };
    first.subscription = evio_rx_subscription_new(loop, source, observer(&first), 2);
    removed.subscription = evio_rx_subscription_new(loop, source, observer(&removed), 2);
    evio_rx_subscription_start(first.subscription);
    evio_rx_subscription_start(removed.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(first.count, 1);
    assert_int_equal(removed.count, 0);
    assert_int_equal(added.count, 0);
    value = 2;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(first.count, 2);
    assert_int_equal(added.count, 1);
    assert_int_equal(added.values[0], 2);
    assert_int_equal(removed.count, 0);
    evio_rx_subject_complete(subject);
    assert_int_equal(first.completed, 1);
    assert_int_equal(added.completed, 1);
    assert_int_equal(removed.completed, 0);
    evio_rx_subscription_free(first.subscription);
    evio_rx_subscription_free(added.subscription);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

static void free_during_publication(subject_log *log)
{
    evio_rx_subject_free(log->subject);
    log->subject = NULL;
    evio_rx_subscription_free(log->subscription);
    log->subscription = NULL;
}

TEST(test_evio_rx_subject_last_handles_freed_from_callback)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    subject_log log = { .subject = subject, .action = free_during_publication };
    log.subscription = evio_rx_subscription_new(loop, source, observer(&log), 1);
    assert_non_null(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_subscription_start(log.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_null(log.subscription);
    assert_null(log.subject);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, 0);
    evio_loop_free(loop);
}

TEST(test_evio_rx_subject_late_subscriber_only_receives_terminal)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int errors[] = {0, EIO, EINVAL};
    for (size_t i = 0; i < 3; ++i) {
        evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 2);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        int value = 7;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        subject_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, source, observer(&log), 2);
        assert_non_null(log.subscription);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, 0);
        assert_int_equal(log.completed, 0);
        if (!i) {
            evio_rx_subject_complete(subject);
        } else {
            evio_rx_subject_error(subject, i == 1 ? EIO : 0);
        }
        evio_rx_subject_complete(subject);
        evio_rx_subject_error(subject, EBUSY);
        assert_int_equal(evio_rx_subject_next(subject, &value), -1);
        assert_int_equal(errno, EPIPE);
        assert_int_equal(log.error, errors[i]);
        assert_int_equal(log.completed, !i);
        subject_log late = {0};
        late.subscription = evio_rx_subscription_new(loop, source, observer(&late), 2);
        assert_non_null(late.subscription);
        assert_int_equal(late.completed, 0);
        evio_rx_subscription_start(late.subscription);
        assert_int_equal(late.count, 0);
        assert_int_equal(late.error, errors[i]);
        assert_int_equal(late.completed, !i);
        evio_rx_subscription_free(late.subscription);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

static void reopen_busy(subject_log *log)
{
    assert_int_equal(evio_rx_subject_reopen(log->subject), -1);
    assert_int_equal(errno, EBUSY);
}

TEST(test_evio_rx_subject_reopen_resumes_restarted_subscribers)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    subject_log log = { .subject = subject, .action = reopen_busy };
    log.subscription = evio_rx_subscription_new(loop, source, observer(&log), 2);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(evio_rx_subject_reopen(subject), 0);
    int values[] = {1, 2, 3};
    assert_int_equal(evio_rx_subject_next(subject, &values[0]), 0);
    evio_rx_subject_error(subject, EIO);
    assert_int_equal(log.error, EIO);
    for (size_t i = 1; i < 3; ++i) {
        assert_int_equal(evio_rx_subject_reopen(subject), 0);
        log.error = 0;
        log.completed = 0;
        evio_rx_subscription_restart(log.subscription);
        assert_false(evio_rx_subscription_is_closed(log.subscription));
        assert_int_equal(evio_rx_subject_next(subject, &values[i]), 0);
        evio_rx_subject_complete(subject);
        assert_int_equal(log.completed, 1);
        assert_int_equal(log.error, 0);
    }
    assert_int_equal(log.count, 3);
    assert_memory_equal(log.values, values, sizeof(values));
    subject_log late = {0};
    late.subscription = evio_rx_subscription_new(loop, source, observer(&late), 2);
    assert_int_equal(evio_rx_subject_reopen(subject), 0);
    evio_rx_subscription_start(late.subscription);
    assert_false(evio_rx_subscription_is_closed(late.subscription));
    assert_int_equal(late.completed, 0);
    evio_rx_subscription_free(late.subscription);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    assert_int_equal(loop->refcount, 0);
    evio_loop_free(loop);
}

// A restarted subscriber is relinked last; a stale successor link corrupted the list.
TEST(test_evio_rx_subject_restarted_subscriber_relinks_last)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 2);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    subject_log first = {0};
    subject_log second = {0};
    first.subscription = evio_rx_subscription_new(loop, source, observer(&first), 2);
    second.subscription = evio_rx_subscription_new(loop, source, observer(&second), 2);
    evio_rx_subscription_start(first.subscription);
    evio_rx_subscription_start(second.subscription);
    evio_rx_subscription_restart(first.subscription);
    evio_rx_subscription_cancel(first.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(first.count, 0);
    assert_int_equal(second.count, 1);
    evio_rx_subscription_free(first.subscription);
    evio_rx_subscription_free(second.subscription);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    assert_int_equal(loop->refcount, 0);
    evio_loop_free(loop);
}

static void overflow_subject(subject_log *log)
{
    if (log->count != 1) {
        return;
    }
    int value = 2;
    assert_int_equal(evio_rx_subject_next(log->subject, &value), 0);
    value = 3;
    assert_int_equal(evio_rx_subject_next(log->subject, &value), -1);
    assert_int_equal(errno, EOVERFLOW);
}

TEST(test_evio_rx_subject_overflow_closes_all_subscribers)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    subject_log first = { .subject = subject, .action = overflow_subject };
    subject_log second = {0};
    first.subscription = evio_rx_subscription_new(loop, source, observer(&first), 2);
    second.subscription = evio_rx_subscription_new(loop, source, observer(&second), 2);
    evio_rx_subscription_start(first.subscription);
    evio_rx_subscription_start(second.subscription);
    int value = 1;
    assert_int_equal(evio_rx_subject_next(subject, &value), 0);
    assert_int_equal(first.count, 1);
    assert_int_equal(second.count, 1);
    assert_int_equal(first.error, EOVERFLOW);
    assert_int_equal(second.error, EOVERFLOW);
    assert_int_equal(first.completed, 0);
    assert_int_equal(second.completed, 0);
    evio_rx_subscription_free(first.subscription);
    evio_rx_subscription_free(second.subscription);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

typedef struct {
    evio_rx_subject *subject;
    size_t copies;
    size_t destroyed;
    bool fail;
    bool reenter;
} payload_log;

static int payload_copy(void *ctx, void *out, const void *value)
{
    payload_log *log = ctx;
    if (log->fail) {
        return EIO;
    }
    if (log->reenter) {
        log->reenter = false;
        int nested = 3;
        assert_int_equal(evio_rx_subject_next(log->subject, &nested), 0);
        return EIO;
    }
    *(int *)out = *(const int *)value;
    log->copies++;
    return 0;
}

static void payload_destroy(void *ctx, void *value)
{
    payload_log *log = ctx;
    log->destroyed++;
}

static void copy_failure(subject_log *log)
{
    if (log->count != 1) {
        return;
    }
    int value = 2;
    assert_int_equal(evio_rx_subject_next(log->subject, &value), -1);
    assert_int_equal(errno, EIO);
}

TEST(test_evio_rx_subject_owned_queue_copy_and_rollback)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 3; ++i) {
        payload_log payload = { .fail = i == 1, .reenter = i == 2 };
        evio_rx_type type = {
            .size = sizeof(int), .alignment = alignof(int), .ctx = &payload,
            .copy = payload_copy, .destroy = payload_destroy,
        };
        evio_rx_subject *subject = evio_rx_subject_new(loop, &type, 2);
        payload.subject = subject;
        evio_rx_observable *source = evio_rx_from_subject(subject);
        subject_log second = {0};
        subject_log first = {
            .subject = subject,
            .other = &second,
            .action = i ? copy_failure : publish_nested,
        };
        first.subscription = evio_rx_subscription_new(loop, source, observer(&first), 2);
        second.subscription = evio_rx_subscription_new(loop, source, observer(&second), 2);
        evio_rx_subscription_start(first.subscription);
        evio_rx_subscription_start(second.subscription);
        int value = 1;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        assert_int_equal(first.count, i ? 1 : 2);
        assert_int_equal(second.count, first.count);
        assert_int_equal(first.error, i ? EIO : 0);
        assert_int_equal(second.error, first.error);
        assert_int_equal(payload.copies, i == 1 ? 0 : 1);
        assert_int_equal(payload.destroyed, payload.copies);
        evio_rx_subscription_free(first.subscription);
        evio_rx_subscription_free(second.subscription);
        evio_rx_observable_free(source);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_subject_loop_binding_and_invalid_configuration)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_loop *other = evio_loop_new(EVIO_FLAG_NONE);
    assert_null(evio_rx_subject_new(NULL, &int_type, 1));
    assert_null(evio_rx_subject_new(loop, NULL, 1));
    assert_null(evio_rx_subject_new(loop, &int_type, 0));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_subject_new(loop, &int_type, SIZE_MAX));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_from_subject(NULL));
    assert_int_equal(errno, EINVAL);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    subject_log log = {0};
    assert_null(evio_rx_subscription_new(other, source, observer(&log), 1));
    assert_int_equal(errno, EINVAL);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.error, 0);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    evio_loop_free(other);
    evio_loop_free(loop);
}

static void other_not_reached(subject_log *log)
{
    assert_int_equal(log->other->count, 0);
}

static int count_batch(void *ctx, void *out, const void *values, size_t count)
{
    size_t *calls = ctx;
    (*calls)++;
    memcpy(out, values, count * sizeof(int));
    return 0;
}

TEST(test_evio_rx_subject_batch_reaches_each_subscriber_whole)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    size_t calls = 0;
    evio_rx_observable *mapped = evio_rx_map_batch(source, &int_type, count_batch,
                                                   (evio_rx_capture) { .data = &calls });
    subject_log second = {0};
    subject_log first = { .other = &second, .action = other_not_reached };
    first.subscription = evio_rx_subscription_new(loop, mapped, observer(&first), 1);
    second.subscription = evio_rx_subscription_new(loop, source, observer(&second), 1);
    evio_rx_subscription_start(first.subscription);
    evio_rx_subscription_start(second.subscription);
    int values[] = {1, 2, 3};
    assert_int_equal(evio_rx_subject_next_batch(subject, values, 3), 0);
    assert_int_equal(evio_rx_subject_next_batch(subject, values, 0), 0);
    assert_int_equal(calls, 1);
    assert_int_equal(first.count, 3);
    assert_memory_equal(first.values, values, sizeof(values));
    assert_int_equal(second.count, 3);
    assert_memory_equal(second.values, values, sizeof(values));
    evio_rx_subject_complete(subject);
    assert_int_equal(evio_rx_subject_next_batch(subject, values, 3), -1);
    assert_int_equal(errno, EPIPE);
    assert_int_equal(first.completed, 1);
    assert_int_equal(second.completed, 1);
    evio_rx_subscription_free(first.subscription);
    evio_rx_subscription_free(second.subscription);
    evio_rx_observable_free(mapped);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

static void publish_nested_batch(subject_log *log)
{
    if (log->count != 1) {
        return;
    }
    int values[] = {2, 3};
    int rc = evio_rx_subject_next_batch(log->subject, values, 2);
    if (log->terminal) {
        assert_int_equal(rc, -1);
        assert_int_equal(errno, EOVERFLOW);
    } else {
        assert_int_equal(rc, 0);
    }
    assert_int_equal(log->count, 1);
}

TEST(test_evio_rx_subject_nested_batch_is_queued_or_overflows)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t capacity = 1; capacity <= 2; ++capacity) {
        evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, capacity);
        evio_rx_observable *source = evio_rx_from_subject(subject);
        subject_log log = {
            .subject = subject, .action = publish_nested_batch, .terminal = capacity == 1,
        };
        log.subscription = evio_rx_subscription_new(loop, source, observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        int value = 1;
        assert_int_equal(evio_rx_subject_next(subject, &value), 0);
        int expected[] = {1, 2, 3};
        assert_int_equal(log.count, capacity == 1 ? 1 : 3);
        assert_memory_equal(log.values, expected, log.count * sizeof(int));
        assert_int_equal(log.error, capacity == 1 ? EOVERFLOW : 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_subject_free(subject);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_subject_nested_publication_follows_batch)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    subject_log second = {0};
    subject_log first = { .subject = subject, .other = &second, .action = publish_nested };
    first.subscription = evio_rx_subscription_new(loop, source, observer(&first), 1);
    second.subscription = evio_rx_subscription_new(loop, source, observer(&second), 1);
    evio_rx_subscription_start(first.subscription);
    evio_rx_subscription_start(second.subscription);
    int values[] = {1, 3};
    assert_int_equal(evio_rx_subject_next_batch(subject, values, 2), 0);
    int expected[] = {1, 3, 2};
    assert_int_equal(first.count, 3);
    assert_memory_equal(first.values, expected, sizeof(expected));
    assert_int_equal(second.count, 3);
    assert_memory_equal(second.values, expected, sizeof(expected));
    evio_rx_subscription_free(first.subscription);
    evio_rx_subscription_free(second.subscription);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}

TEST(test_evio_rx_subject_batch_without_values_fails_subject)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_from_subject(subject);
    subject_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, source, observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(evio_rx_subject_next_batch(subject, NULL, 2), -1);
    assert_int_equal(errno, EINVAL);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.error, EINVAL);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_subject_free(subject);
    evio_loop_free(loop);
}
