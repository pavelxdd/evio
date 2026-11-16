#include "test.h"
#include "evio_rx.h"

static const evio_rx_type int_type = {
    .size = sizeof(int), .alignment = alignof(int),
};

typedef struct producer_state producer_state;

typedef struct {
    evio_rx_emitter *emitters[4];
    producer_state *states[4];
    size_t attempts;
    size_t prepared;
    size_t started;
    size_t stopped;
    size_t destroyed;
    size_t captures;
    size_t burst;
    int prepare_error;
    bool timer;
    bool after_next;
} producer_log;

struct producer_state {
    producer_log *log;
    evio_rx_emitter *emitter;
    evio_loop *loop;
    evio_timer timer;
    size_t index;
    int position;
    bool started;
    bool stopped;
};

static void emit_value(evio_rx_emitter *emitter, void *state, void *data)
{
    evio_rx_emitter_next(emitter, data);
}

static void emit_complete(evio_rx_emitter *emitter, void *state, void *data)
{
    evio_rx_emitter_complete(emitter);
    assert_false(evio_rx_emitter_next(emitter, data));
}

static void emit_error(evio_rx_emitter *emitter, void *state, void *data)
{
    evio_rx_emitter_error(emitter, *(int *)data);
    assert_false(evio_rx_emitter_next(emitter, data));
}

static void publish(evio_rx_emitter *emitter, int value)
{
    evio_rx_emitter_invoke(emitter, emit_value, &value);
}

static void inspect_after_next(evio_rx_emitter *emitter, void *state, void *data)
{
    producer_state *producer = state;
    producer_log *log = producer->log;
    int value = 1;
    assert_false(evio_rx_emitter_next(emitter, &value));
    assert_true(producer->stopped);
    assert_int_equal(log->stopped, 1);
    assert_int_equal(log->destroyed, 0);
    producer->position = 42;
    log->after_next = true;
}

static void producer_timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    producer_state *producer = base->data;
    evio_rx_emitter_invoke(producer->emitter, inspect_after_next, NULL);
}

static int producer_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    producer_log *log = ctx;
    assert_null(*state);
    log->attempts++;
    producer_state *producer = evio_calloc(1, sizeof(*producer));
    if (log->prepare_error) {
        evio_free(producer);
        return log->prepare_error;
    }
    size_t index = log->prepared++;
    assert_true(index < 4);
    producer->log = log;
    producer->emitter = emitter;
    producer->loop = loop;
    producer->index = index;
    evio_timer_init(&producer->timer, producer_timer_cb, 0);
    producer->timer.data = producer;
    log->emitters[index] = emitter;
    log->states[index] = producer;
    *state = producer;
    return 0;
}

static void producer_start(evio_rx_emitter *emitter, void *state)
{
    producer_state *producer = state;
    producer_log *log = producer->log;
    producer->started = true;
    log->started++;
    if (log->timer) {
        evio_timer_start(producer->loop, &producer->timer, 0);
    }
    for (size_t i = 0; i < log->burst; ++i) {
        producer->position++;
        if (!evio_rx_emitter_next(emitter, &producer->position)) {
            return;
        }
    }
    if (log->burst) {
        evio_rx_emitter_complete(emitter);
        assert_true(producer->stopped);
        assert_int_equal(log->destroyed, log->started - 1);
    }
}

static void producer_stop(void *state)
{
    producer_state *producer = state;
    assert_false(producer->stopped);
    producer->stopped = true;
    producer->log->stopped++;
    producer->log->emitters[producer->index] = NULL;
    evio_timer_stop(producer->loop, &producer->timer);
}

static void producer_destroy(void *state)
{
    producer_state *producer = state;
    assert_true(!producer->started || producer->stopped);
    producer->log->destroyed++;
    producer->log->states[producer->index] = NULL;
    producer->log->emitters[producer->index] = NULL;
    evio_free(producer);
}

static void capture_destroy(void *ctx)
{
    producer_log *log = ctx;
    log->captures++;
}

static const evio_rx_source producer_source = {
    .prepare = producer_prepare,
    .start = producer_start,
    .stop = producer_stop,
    .destroy = producer_destroy,
};

typedef struct user_log user_log;
struct user_log {
    producer_log *producer;
    evio_rx_subscription *subscription;
    int values[32];
    size_t count;
    size_t completed;
    int error;
    size_t depth;
    bool free_next;
    bool cancel_next;
    bool cancel_terminal;
    void (*on_next)(user_log *log);
};

static void user_next(void *ctx, const void *value)
{
    user_log *log = ctx;
    assert_int_equal(log->depth++, 0);
    assert_true(log->count < 32);
    log->values[log->count++] = *(const int *)value;
    if (log->on_next) {
        log->on_next(log);
    }
    if (log->free_next) {
        evio_rx_subscription_free(log->subscription);
        log->subscription = NULL;
    } else if (log->cancel_next) {
        evio_rx_subscription_cancel(log->subscription);
    }
    log->depth--;
}

static void user_error(void *ctx, int error)
{
    user_log *log = ctx;
    assert_int_equal(log->depth, 0);
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->error = error;
    if (log->cancel_terminal) {
        evio_rx_subscription_cancel(log->subscription);
    }
}

static void user_complete(void *ctx)
{
    user_log *log = ctx;
    assert_int_equal(log->depth, 0);
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->completed++;
    if (log->cancel_terminal) {
        evio_rx_subscription_cancel(log->subscription);
    }
}

static evio_rx_observer user_observer(user_log *log)
{
    return (evio_rx_observer) {
        .next = user_next, .error = user_error, .complete = user_complete, .ctx = log,
    };
}

TEST(test_evio_rx_user_source_independent_synchronous_bursts)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    producer_log producer = { .burst = 8 };
    user_log logs[2] = {0};
    evio_rx_observable *source = evio_rx_observable_new(&int_type, producer_source,
                                                        (evio_rx_capture) { .data = &producer, .destroy = capture_destroy });
    assert_non_null(source);
    for (size_t i = 0; i < 2; ++i) {
        logs[i].subscription = evio_rx_subscription_new(loop, source, user_observer(&logs[i]), 1);
        assert_non_null(logs[i].subscription);
        assert_int_equal(logs[i].count, 0);
    }
    assert_ptr_not_equal(producer.states[0], producer.states[1]);
    assert_int_equal(producer.started, 0);
    evio_rx_observable_free(source);
    for (size_t i = 0; i < 2; ++i) {
        evio_rx_subscription_start(logs[i].subscription);
        int expected[] = {1, 2, 3, 4, 5, 6, 7, 8};
        assert_int_equal(logs[i].count, 8);
        assert_memory_equal(logs[i].values, expected, sizeof(expected));
        assert_int_equal(logs[i].completed, 1);
        assert_int_equal(logs[i].error, 0);
        assert_int_equal(producer.destroyed, i + 1);
        evio_rx_subscription_start(logs[i].subscription);
        evio_rx_subscription_cancel(logs[i].subscription);
        evio_rx_subscription_free(logs[i].subscription);
    }
    assert_int_equal(producer.started, 2);
    assert_int_equal(producer.stopped, 2);
    assert_int_equal(producer.captures, 1);
    evio_loop_free(loop);
}

TEST(test_evio_rx_user_source_restart_prepares_new_state)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    producer_log producer = {0};
    user_log log = {0};
    evio_rx_observable *source = evio_rx_observable_new(&int_type, producer_source,
                                                        (evio_rx_capture) { .data = &producer, .destroy = capture_destroy });
    log.subscription = evio_rx_subscription_new(loop, source, user_observer(&log), 1);
    assert_non_null(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_subscription_start(log.subscription);
    evio_rx_subscription_restart(log.subscription);
    assert_int_equal(producer.prepared, 2);
    assert_int_equal(producer.started, 2);
    assert_int_equal(producer.stopped, 1);
    assert_int_equal(producer.destroyed, 1);
    evio_rx_subscription_free(log.subscription);
    assert_int_equal(producer.stopped, 2);
    assert_int_equal(producer.destroyed, 2);
    assert_int_equal(producer.captures, 1);
    evio_loop_free(loop);
}

TEST(test_evio_rx_user_source_restart_prepare_failure_errors_run)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    producer_log producer = {0};
    user_log log = {0};
    evio_rx_observable *source = evio_rx_observable_new(&int_type, producer_source,
                                                        (evio_rx_capture) { .data = &producer });
    log.subscription = evio_rx_subscription_new(loop, source, user_observer(&log), 1);
    assert_non_null(log.subscription);
    evio_rx_subscription_start(log.subscription);
    producer.prepare_error = EIO;
    evio_rx_subscription_restart(log.subscription);
    assert_true(evio_rx_subscription_is_closed(log.subscription));
    assert_int_equal(log.error, EIO);
    assert_int_equal(log.completed, 0);
    assert_int_equal(producer.attempts, 2);
    assert_int_equal(producer.prepared, 1);
    assert_int_equal(producer.started, 1);
    assert_int_equal(producer.stopped, 1);
    assert_int_equal(producer.destroyed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

TEST(test_evio_rx_user_source_prepare_failure_and_new_cancel)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    producer_log producer = { .prepare_error = EIO };
    user_log log = {0};
    evio_rx_observable *source = evio_rx_observable_new(&int_type, producer_source,
                                                        (evio_rx_capture) { .data = &producer });
    assert_non_null(source);
    assert_null(evio_rx_subscription_new(loop, source, user_observer(&log), 2));
    assert_int_equal(errno, EIO);
    assert_int_equal(producer.attempts, 1);
    assert_int_equal(producer.prepared, 0);
    assert_int_equal(producer.destroyed, 0);
    producer.prepare_error = 0;
    log.subscription = evio_rx_subscription_new(loop, source, user_observer(&log), 2);
    assert_non_null(log.subscription);
    evio_rx_subscription_cancel(log.subscription);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(producer.prepared, 1);
    assert_int_equal(producer.started, 0);
    assert_int_equal(producer.stopped, 0);
    assert_int_equal(producer.destroyed, 1);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.error, 0);
    assert_int_equal(log.completed, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    assert_int_equal(producer.captures, 0);
    evio_loop_free(loop);
}

TEST(test_evio_rx_user_source_invoke_holds_state_after_free)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        producer_log producer = {0};
        user_log log = { .free_next = i, .cancel_next = !i };
        evio_rx_observable *source = evio_rx_observable_new(&int_type, producer_source,
                                                            (evio_rx_capture) { .data = &producer, .destroy = capture_destroy });
        log.subscription = evio_rx_subscription_new(loop, source, user_observer(&log), 1);
        assert_non_null(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_subscription_start(log.subscription);
        evio_rx_emitter_invoke(producer.emitters[0], inspect_after_next, NULL);
        assert_true(producer.after_next);
        assert_int_equal(producer.stopped, 1);
        assert_int_equal(producer.destroyed, 1);
        assert_int_equal(log.count, 1);
        assert_int_equal(log.completed, 0);
        assert_int_equal(log.error, 0);
        if (log.subscription) {
            assert_int_equal(producer.captures, 0);
            evio_rx_subscription_free(log.subscription);
        }
        assert_int_equal(producer.captures, 1);
    }
    evio_loop_free(loop);
}

static void reenter_with_terminal(user_log *log)
{
    if (log->count != 1) {
        return;
    }
    evio_rx_emitter *emitter = log->producer->emitters[0];
    int value = 2;
    evio_rx_emitter_invoke(emitter, emit_value, &value);
    value = 99;
    int error = log->producer->prepare_error;
    if (error) {
        evio_rx_emitter_invoke(emitter, emit_error, &error);
    } else {
        evio_rx_emitter_invoke(emitter, emit_complete, NULL);
    }
    assert_int_equal(log->producer->stopped, 1);
    assert_int_equal(log->producer->destroyed, 0);
    assert_int_equal(log->count, 1);
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
}

TEST(test_evio_rx_user_source_reentrant_values_precede_terminal)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        producer_log producer = {0};
        user_log log = {
            .producer = &producer,
            .on_next = reenter_with_terminal,
            .cancel_terminal = true,
        };
        evio_rx_observable *source = evio_rx_observable_new(&int_type, producer_source,
                                                            (evio_rx_capture) { .data = &producer });
        log.subscription = evio_rx_subscription_new(loop, source, user_observer(&log), 2);
        assert_non_null(log.subscription);
        evio_rx_subscription_start(log.subscription);
        producer.prepare_error = i ? EIO : 0;
        publish(producer.emitters[0], 1);
        int expected[] = {1, 2};
        assert_int_equal(log.count, 2);
        assert_memory_equal(log.values, expected, sizeof(expected));
        assert_int_equal(log.completed, !i);
        assert_int_equal(log.error, i ? EIO : 0);
        assert_int_equal(producer.destroyed, 1);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_user_source_terminal_queue_overflow_discards_backlog)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    producer_log producer = {0};
    user_log log = { .producer = &producer, .on_next = reenter_with_terminal };
    evio_rx_observable *source = evio_rx_observable_new(&int_type, producer_source,
                                                        (evio_rx_capture) { .data = &producer });
    log.subscription = evio_rx_subscription_new(loop, source, user_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    publish(producer.emitters[0], 1);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.values[0], 1);
    assert_int_equal(log.error, EOVERFLOW);
    assert_int_equal(log.completed, 0);
    assert_int_equal(producer.stopped, 1);
    assert_int_equal(producer.destroyed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

TEST(test_evio_rx_user_source_cancel_suppresses_queued_terminal)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    producer_log producer = {0};
    user_log log = { .producer = &producer, .on_next = reenter_with_terminal, .cancel_next = true };
    evio_rx_observable *source = evio_rx_observable_new(&int_type, producer_source,
                                                        (evio_rx_capture) { .data = &producer });
    log.subscription = evio_rx_subscription_new(loop, source, user_observer(&log), 2);
    evio_rx_subscription_start(log.subscription);
    publish(producer.emitters[0], 1);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(producer.stopped, 1);
    assert_int_equal(producer.destroyed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

static void watchdog_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    fail_msg("Rx producer watchdog expired");
}

TEST(test_evio_rx_user_source_native_timer_free_and_pending_cancel)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    producer_log producer = { .timer = true };
    user_log log = { .free_next = true };
    evio_rx_observable *source = evio_rx_observable_new(&int_type, producer_source,
                                                        (evio_rx_capture) { .data = &producer });
    log.subscription = evio_rx_subscription_new(loop, source, user_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    evio_timer watchdog;
    evio_timer_init(&watchdog, watchdog_cb, 0);
    evio_timer_start(loop, &watchdog, EVIO_TIME_FROM_SEC(5));
    evio_unref(loop);
    evio_run(loop, 0);
    evio_ref(loop);
    evio_timer_stop(loop, &watchdog);
    assert_true(producer.after_next);
    assert_int_equal(producer.destroyed, 1);
    assert_int_equal(log.count, 1);
    assert_null(log.subscription);
    assert_int_equal(loop->refcount, 0);

    user_log pending = {0};
    pending.subscription = evio_rx_subscription_new(loop, source, user_observer(&pending), 1);
    evio_rx_subscription_start(pending.subscription);
    evio_timer_update(loop);
    assert_true(producer.states[1]->timer.pending != 0);
    evio_rx_subscription_cancel(pending.subscription);
    evio_invoke_pending(loop);
    assert_int_equal(pending.count, 0);
    assert_int_equal(producer.destroyed, 2);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(pending.subscription);
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

typedef struct {
    alignas(64) int value;
    const void *self;
} owned_value;

typedef struct {
    producer_log *producer;
    size_t copies;
    size_t destroyed;
    bool fail_copy;
    bool reenter_destroy;
} value_log;

static int value_copy(void *ctx, void *out, const void *value)
{
    value_log *log = ctx;
    assert_int_equal((uintptr_t)out % 64, 0);
    const owned_value *input = value;
    assert_ptr_equal(input->self, input);
    if (log->fail_copy) {
        return EIO;
    }
    owned_value *result = out;
    result->value = input->value;
    result->self = result;
    log->copies++;
    return 0;
}

static void value_destroy(void *ctx, void *value)
{
    value_log *log = ctx;
    owned_value *input = value;
    assert_ptr_equal(input->self, input);
    assert_int_equal((uintptr_t)value % 64, 0);
    log->destroyed++;
    if (log->reenter_destroy) {
        log->reenter_destroy = false;
        int previous = input->value;
        publish(log->producer->emitters[0], 2);
        assert_int_equal(input->value, previous);
        assert_ptr_equal(input->self, input);
    }
}

static void owned_next(void *ctx, const void *value)
{
    const owned_value *input = value;
    assert_int_equal((uintptr_t)value % 64, 0);
    assert_ptr_equal(input->self, input);
    user_next(ctx, &input->value);
}

static void reenter_owned(user_log *log)
{
    if (log->count != 1) {
        return;
    }
    owned_value value = { .value = 2 };
    value.self = &value;
    evio_rx_emitter_invoke(log->producer->emitters[0], emit_value, &value);
    value.value = 99;
}

TEST(test_evio_rx_user_source_reentrant_owned_copy_and_failure)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        producer_log producer = {0};
        value_log values = { .fail_copy = i };
        evio_rx_type type = {
            .size = sizeof(owned_value), .alignment = alignof(owned_value),
            .copy = value_copy, .destroy = value_destroy, .ctx = &values,
        };
        user_log log = { .producer = &producer, .on_next = reenter_owned };
        evio_rx_observer callbacks = user_observer(&log);
        callbacks.next = owned_next;
        evio_rx_observable *source = evio_rx_observable_new(&type, producer_source,
                                                            (evio_rx_capture) { .data = &producer });
        log.subscription = evio_rx_subscription_new(loop, source, callbacks, 2);
        evio_rx_subscription_start(log.subscription);
        owned_value value = { .value = 1 };
        value.self = &value;
        evio_rx_emitter_invoke(producer.emitters[0], emit_value, &value);
        assert_int_equal(log.count, i ? 1 : 2);
        assert_int_equal(log.error, i ? EIO : 0);
        assert_int_equal(log.values[0], 1);
        if (!i) {
            assert_int_equal(log.values[1], 2);
        }
        evio_rx_subscription_free(log.subscription);
        assert_int_equal(values.copies, i ? 0 : 1);
        assert_int_equal(values.destroyed, values.copies);
        evio_rx_observable_free(source);
    }
    evio_loop_free(loop);
}

static int create_owned(void *ctx, void *out, const void *value)
{
    value_log *log = ctx;
    owned_value *result = out;
    assert_int_equal((uintptr_t)out % 64, 0);
    result->value = *(const int *)value;
    result->self = result;
    log->copies++;
    return 0;
}

TEST(test_evio_rx_user_source_map_destroy_reentry_preserves_output)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    producer_log producer = {0};
    value_log values = { .producer = &producer, .reenter_destroy = true };
    evio_rx_type type = {
        .size = sizeof(owned_value), .alignment = alignof(owned_value),
        .copy = value_copy, .destroy = value_destroy, .ctx = &values,
    };
    user_log log = {0};
    evio_rx_observer callbacks = user_observer(&log);
    callbacks.next = owned_next;
    evio_rx_observable *source = evio_rx_observable_new(&int_type, producer_source,
                                                        (evio_rx_capture) { .data = &producer });
    evio_rx_observable *mapped = evio_rx_map(source, &type, create_owned,
                                             (evio_rx_capture) { .data = &values });
    log.subscription = evio_rx_subscription_new(loop, mapped, callbacks, 2);
    evio_rx_observable_free(source);
    evio_rx_observable_free(mapped);
    evio_rx_subscription_start(log.subscription);
    publish(producer.emitters[0], 1);
    int expected[] = {1, 2};
    assert_int_equal(log.count, 2);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(values.copies, 2);
    assert_int_equal(values.destroyed, 2);
    evio_rx_emitter_invoke(producer.emitters[0], emit_complete, NULL);
    assert_int_equal(log.completed, 1);
    assert_int_equal(producer.destroyed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_loop_free(loop);
}

typedef struct backend_adapter backend_adapter;

typedef struct {
    backend_adapter *adapter;
    size_t stopped;
    size_t destroyed;
    size_t released;
} backend_log;

struct backend_adapter {
    backend_log *log;
    evio_rx_emitter *emitter;
    size_t refs;
};

static void backend_release(backend_adapter *adapter)
{
    if (--adapter->refs) {
        return;
    }
    adapter->log->released++;
    adapter->log->adapter = NULL;
    evio_free(adapter);
}

static int backend_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    backend_log *log = ctx;
    backend_adapter *adapter = evio_calloc(1, sizeof(*adapter));
    adapter->log = log;
    adapter->emitter = emitter;
    adapter->refs = 2;
    log->adapter = adapter;
    *state = adapter;
    return 0;
}

static void backend_start(evio_rx_emitter *emitter, void *state)
{
}

static void backend_stop(void *state)
{
    backend_adapter *adapter = state;
    adapter->log->stopped++;
    adapter->emitter = NULL;
}

static void backend_destroy(void *state)
{
    backend_adapter *adapter = state;
    adapter->log->destroyed++;
    backend_release(adapter);
}

static void backend_complete(backend_adapter *adapter)
{
    if (adapter->emitter) {
        evio_rx_emitter_invoke(adapter->emitter, emit_complete, NULL);
    }
    backend_release(adapter);
}

TEST(test_evio_rx_user_source_uncancellable_backend_detaches)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    backend_log backend = {0};
    user_log log = {0};
    evio_rx_source hooks = {
        .prepare = backend_prepare, .start = backend_start,
        .stop = backend_stop, .destroy = backend_destroy,
    };
    evio_rx_observable *source = evio_rx_observable_new(&int_type, hooks,
                                                        (evio_rx_capture) { .data = &backend });
    log.subscription = evio_rx_subscription_new(loop, source, user_observer(&log), 1);
    assert_non_null(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_subscription_start(log.subscription);
    evio_rx_subscription_free(log.subscription);
    log.subscription = NULL;
    assert_int_equal(backend.stopped, 1);
    assert_int_equal(backend.destroyed, 1);
    assert_int_equal(backend.released, 0);
    assert_null(backend.adapter->emitter);
    backend_complete(backend.adapter);
    assert_null(backend.adapter);
    assert_int_equal(backend.released, 1);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, 0);
    evio_loop_free(loop);
}

static int stateless_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    assert_null(*state);
    return 0;
}

static void forbidden_invoke(evio_rx_emitter *emitter, void *state, void *data)
{
    fail_msg("closed emitter invoked producer");
}

static void stateless_start(evio_rx_emitter *emitter, void *state)
{
    assert_null(state);
    evio_rx_emitter_complete(emitter);
    evio_rx_emitter_invoke(emitter, forbidden_invoke, NULL);
}

TEST(test_evio_rx_user_source_stateless_optional_hooks)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    user_log log = {0};
    evio_rx_source hooks = { .prepare = stateless_prepare, .start = stateless_start };
    evio_rx_observable *source = evio_rx_observable_new(&int_type, hooks, (evio_rx_capture) {0});
    log.subscription = evio_rx_subscription_new(loop, source, user_observer(&log), 1);
    assert_non_null(log.subscription);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.count, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

TEST(test_evio_rx_user_source_runtime_error_normalization)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int errors[] = {EIO, 0, -EIO};
    for (size_t i = 0; i < 4; ++i) {
        producer_log producer = {0};
        user_log log = {0};
        evio_rx_observable *source = evio_rx_observable_new(&int_type, producer_source,
                                                            (evio_rx_capture) { .data = &producer });
        log.subscription = evio_rx_subscription_new(loop, source, user_observer(&log), 1);
        assert_non_null(log.subscription);
        evio_rx_subscription_start(log.subscription);
        if (i < 3) {
            evio_rx_emitter_invoke(producer.emitters[0], emit_error, &errors[i]);
        } else {
            evio_rx_emitter_invoke(producer.emitters[0], emit_value, NULL);
        }
        assert_int_equal(log.error, i ? EINVAL : EIO);
        assert_int_equal(log.count, 0);
        assert_int_equal(log.completed, 0);
        assert_int_equal(producer.stopped, 1);
        assert_int_equal(producer.destroyed, 1);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
    }
    evio_loop_free(loop);
}

typedef struct {
    evio_loop *loop;
    evio_rx_subscription *subscription;
    evio_timer *watchdog;
    uint64_t values[8];
    size_t count;
    size_t completed;
    size_t cancel_at;
    evio_time advance_to;
    int error;
    bool free_next;
} timer_log;

static void timer_next(void *ctx, const void *value)
{
    timer_log *log = ctx;
    assert_true(log->count < 8);
    log->values[log->count++] = *(const uint64_t *)value;
    if (log->advance_to) {
        log->loop->time = log->advance_to;
    }
    if (log->free_next) {
        evio_rx_subscription_free(log->subscription);
        log->subscription = NULL;
    } else if (log->cancel_at == log->count) {
        evio_rx_subscription_cancel(log->subscription);
    }
    if (log->watchdog && (log->free_next || log->cancel_at == log->count)) {
        evio_timer_stop(log->loop, log->watchdog);
    }
}

static void timer_complete(void *ctx)
{
    timer_log *log = ctx;
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->completed++;
    if (log->watchdog) {
        evio_timer_stop(log->loop, log->watchdog);
    }
}

static void timer_error(void *ctx, int error)
{
    timer_log *log = ctx;
    assert_int_equal(log->error, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->error = error;
    if (log->watchdog) {
        evio_timer_stop(log->loop, log->watchdog);
    }
}

static evio_rx_observer timer_observer(timer_log *log)
{
    return (evio_rx_observer) {
        .next = timer_next, .error = timer_error, .complete = timer_complete, .ctx = log,
    };
}

static void timer_advance(evio_loop *loop, evio_time time)
{
    loop->time = time;
    evio_timer_update(loop);
    evio_invoke_pending(loop);
}

TEST(test_evio_rx_timer_zero_is_async_and_pending_cancel_is_independent)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_observable *source = evio_rx_timer(0);
    assert_non_null(source);
    timer_log first = {0};
    timer_log second = {0};
    first.subscription = evio_rx_subscription_new(loop, source, timer_observer(&first), 1);
    second.subscription = evio_rx_subscription_new(loop, source, timer_observer(&second), 1);
    assert_non_null(first.subscription);
    assert_non_null(second.subscription);
    assert_int_equal(loop->refcount, 0);
    evio_rx_observable_free(source);
    evio_rx_subscription_start(first.subscription);
    evio_rx_subscription_start(second.subscription);
    assert_int_equal(first.count, 0);
    assert_int_equal(second.count, 0);
    assert_int_equal(loop->refcount, 2);
    evio_timer_update(loop);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_cancel(second.subscription);
    evio_invoke_pending(loop);
    assert_int_equal(first.count, 1);
    assert_int_equal(first.values[0], 0);
    assert_int_equal(first.completed, 1);
    assert_int_equal(second.count, 0);
    assert_int_equal(second.completed, 0);
    evio_rx_subscription_free(first.subscription);
    evio_rx_subscription_free(second.subscription);
    evio_loop_free(loop);
}

TEST(test_evio_rx_timer_deadline_and_time_overflow)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_time bases[] = {100, EVIO_TIME_MAX - 2, EVIO_TIME_MAX - 2};
    evio_time afters[] = {10, 2, 3};
    for (size_t i = 0; i < 3; ++i) {
        loop->time = bases[i];
        timer_log log = {0};
        evio_rx_observable *source = evio_rx_timer(afters[i]);
        log.subscription = evio_rx_subscription_new(loop, source, timer_observer(&log), 1);
        evio_rx_observable_free(source);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, 0);
        assert_int_equal(log.error, i == 2 ? EOVERFLOW : 0);
        if (i != 2) {
            timer_advance(loop, bases[i] + afters[i] - 1);
            assert_int_equal(log.count, 0);
            timer_advance(loop, bases[i] + afters[i]);
            assert_int_equal(log.count, 1);
            assert_int_equal(log.values[0], 0);
            assert_int_equal(log.completed, 1);
        }
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_timer_cancel_before_deadline_and_free_in_next)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_observable *source = evio_rx_timer(10);
    timer_log cancelled = {0};
    cancelled.subscription = evio_rx_subscription_new(loop, source, timer_observer(&cancelled), 1);
    evio_rx_subscription_start(cancelled.subscription);
    evio_rx_subscription_cancel(cancelled.subscription);
    timer_advance(loop, 110);
    assert_int_equal(cancelled.count, 0);
    assert_int_equal(cancelled.completed, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(cancelled.subscription);
    timer_log freed = { .free_next = true };
    freed.subscription = evio_rx_subscription_new(loop, source, timer_observer(&freed), 1);
    evio_rx_subscription_start(freed.subscription);
    timer_advance(loop, 120);
    assert_int_equal(freed.count, 1);
    assert_int_equal(freed.completed, 0);
    assert_null(freed.subscription);
    assert_int_equal(loop->refcount, 0);
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

TEST(test_evio_rx_interval_independent_ticks_skip_missed_periods)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_observable *source = evio_rx_interval(0, 5);
    assert_non_null(source);
    timer_log first = { .cancel_at = 3 };
    timer_log second = { .cancel_at = 2 };
    first.subscription = evio_rx_subscription_new(loop, source, timer_observer(&first), 1);
    second.subscription = evio_rx_subscription_new(loop, source, timer_observer(&second), 1);
    evio_rx_observable_free(source);
    evio_rx_subscription_start(first.subscription);
    evio_rx_subscription_start(second.subscription);
    assert_int_equal(first.count, 0);
    assert_int_equal(second.count, 0);
    timer_advance(loop, 100);
    assert_int_equal(first.count, 1);
    assert_int_equal(second.count, 1);
    assert_int_equal(loop->refcount, 2);
    timer_advance(loop, 150);
    assert_int_equal(first.count, 2);
    assert_int_equal(second.count, 2);
    assert_int_equal(loop->refcount, 1);
    timer_advance(loop, 154);
    assert_int_equal(first.count, 2);
    timer_advance(loop, 155);
    uint64_t expected[] = {0, 1, 2};
    assert_int_equal(first.count, 3);
    assert_memory_equal(first.values, expected, sizeof(expected));
    assert_memory_equal(second.values, expected, 2 * sizeof(uint64_t));
    assert_int_equal(first.completed, 0);
    assert_int_equal(second.completed, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(first.subscription);
    evio_rx_subscription_free(second.subscription);
    evio_loop_free(loop);
}

TEST(test_evio_rx_interval_restart_counts_from_zero)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    evio_rx_observable *source = evio_rx_interval(0, 5);
    timer_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, source, timer_observer(&log), 1);
    evio_rx_observable_free(source);
    evio_rx_subscription_start(log.subscription);
    timer_advance(loop, 100);
    timer_advance(loop, 105);
    assert_int_equal(log.count, 2);
    evio_rx_subscription_restart(log.subscription);
    assert_int_equal(loop->refcount, 1);
    timer_advance(loop, 110);
    uint64_t expected[] = {0, 1, 0};
    assert_int_equal(log.count, 3);
    assert_memory_equal(log.values, expected, sizeof(expected));
    evio_rx_subscription_free(log.subscription);
    assert_int_equal(loop->refcount, 0);
    evio_loop_free(loop);
}

TEST(test_evio_rx_interval_time_overflow_and_callback_clock_change)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 3; ++i) {
        loop->time = i == 2 ? 100 : EVIO_TIME_MAX - 2;
        timer_log log = { .loop = loop, .advance_to = i == 2 ? EVIO_TIME_MAX : 0 };
        evio_rx_observable *source = evio_rx_interval(i ? 0 : 3, i == 1 ? 2 : 5);
        log.subscription = evio_rx_subscription_new(loop, source, timer_observer(&log), 1);
        evio_rx_observable_free(source);
        evio_rx_subscription_start(log.subscription);
        if (i) {
            timer_advance(loop, loop->time);
            assert_int_equal(log.count, 1);
            if (i == 1) {
                assert_int_equal(log.error, 0);
                timer_advance(loop, EVIO_TIME_MAX);
                assert_int_equal(log.count, 2);
            }
        }
        assert_int_equal(log.error, EOVERFLOW);
        assert_int_equal(log.completed, 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
    }
    assert_null(evio_rx_interval(0, 0));
    assert_int_equal(errno, EINVAL);
    evio_loop_free(loop);
}

TEST(test_evio_rx_interval_pending_cancel)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    timer_log log = {0};
    evio_rx_observable *source = evio_rx_interval(0, 5);
    log.subscription = evio_rx_subscription_new(loop, source, timer_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    evio_timer_update(loop);
    evio_rx_subscription_cancel(log.subscription);
    evio_invoke_pending(loop);
    assert_int_equal(log.count, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

TEST(test_evio_rx_timer_and_interval_real_loop)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        evio_timer watchdog;
        evio_timer_init(&watchdog, watchdog_cb, 0);
        timer_log log = { .loop = loop, .watchdog = &watchdog, .cancel_at = i ? 3 : 0 };
        evio_rx_observable *source = i ? evio_rx_interval(0, EVIO_TIME_FROM_MSEC(1)) : evio_rx_timer(0);
        log.subscription = evio_rx_subscription_new(loop, source, timer_observer(&log), 1);
        evio_rx_observable_free(source);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, 0);
        evio_timer_start(loop, &watchdog, EVIO_TIME_FROM_SEC(5));
        evio_run(loop, 0);
        assert_int_equal(log.count, i ? 3 : 1);
        assert_int_equal(log.values[0], 0);
        assert_int_equal(log.completed, !i);
        assert_int_equal(log.error, 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
    }
    evio_loop_free(loop);
}

typedef struct {
    evio_loop *loop;
    evio_rx_subscription *subscription;
    evio_timer *watchdog;
    size_t *remaining;
    int fd;
    size_t count;
    evio_mask events;
    int error;
    bool consume;
    bool cancel;
    bool free_next;
    char byte;
} poll_log;

static void poll_next(void *ctx, const void *value)
{
    poll_log *log = ctx;
    const evio_rx_poll_event *event = value;
    assert_int_equal(event->fd, log->fd);
    assert_true(event->events != 0);
    assert_int_equal(event->events & ~(EVIO_READ | EVIO_WRITE), 0);
    log->events = event->events;
    log->count++;
    if (log->consume) {
        assert_int_equal(read(event->fd, &log->byte, 1), 1);
    }
    if (log->free_next) {
        evio_rx_subscription_free(log->subscription);
        log->subscription = NULL;
    } else if (log->cancel) {
        evio_rx_subscription_cancel(log->subscription);
    }
    if (log->remaining) {
        assert_true(*log->remaining > 0);
        if (!-- * log->remaining) {
            evio_timer_stop(log->loop, log->watchdog);
        }
    }
}

static void poll_error(void *ctx, int error)
{
    poll_log *log = ctx;
    assert_int_equal(log->error, 0);
    log->error = error;
    if (log->watchdog) {
        evio_timer_stop(log->loop, log->watchdog);
    }
}

static evio_rx_observer poll_observer(poll_log *log)
{
    return (evio_rx_observer) { .next = poll_next, .error = poll_error, .ctx = log };
}

TEST(test_evio_rx_poll_real_pipe_and_socketpair_readiness)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        int fds[2];
        if (i) {
            assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        } else {
            assert_int_equal(pipe(fds), 0);
            assert_int_equal(write(fds[1], "x", 1), 1);
        }
        evio_timer watchdog;
        evio_timer_init(&watchdog, watchdog_cb, 0);
        size_t remaining = 1;
        poll_log log = {
            .loop = loop, .fd = fds[0], .consume = !i, .free_next = true,
            .watchdog = &watchdog, .remaining = &remaining,
        };
        evio_rx_observable *source = evio_rx_from_poll(fds[0], i ? EVIO_WRITE : EVIO_READ);
        assert_non_null(source);
        log.subscription = evio_rx_subscription_new(loop, source, poll_observer(&log), 1);
        assert_non_null(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, 0);
        evio_timer_start(loop, &watchdog, EVIO_TIME_FROM_SEC(5));
        evio_run(loop, 0);
        assert_null(log.subscription);
        assert_int_equal(log.count, 1);
        assert_int_equal(log.events, i ? EVIO_WRITE : EVIO_READ);
        assert_int_equal(log.error, 0);
        assert_int_equal(remaining, 0);
        if (!i) {
            assert_int_equal(log.byte, 'x');
        }
        assert_true(fcntl(fds[0], F_GETFD) >= 0);
        assert_int_equal(loop->refcount, 0);
        close(fds[0]);
        close(fds[1]);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_poll_multiple_subscriptions_do_not_consume_bytes)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int fds[2];
    assert_int_equal(pipe(fds), 0);
    assert_int_equal(write(fds[1], "x", 1), 1);
    evio_rx_observable *source = evio_rx_from_poll(fds[0], EVIO_READ);
    evio_timer watchdog;
    evio_timer_init(&watchdog, watchdog_cb, 0);
    size_t remaining = 2;
    poll_log logs[2] = {0};
    for (size_t i = 0; i < 2; ++i) {
        logs[i] = (poll_log) {
            .loop = loop,
            .fd = fds[0],
            .cancel = true,
            .watchdog = &watchdog,
            .remaining = &remaining,
        };
        logs[i].subscription = evio_rx_subscription_new(loop, source, poll_observer(&logs[i]), 1);
        evio_rx_subscription_start(logs[i].subscription);
    }
    evio_timer_start(loop, &watchdog, EVIO_TIME_FROM_SEC(5));
    evio_run(loop, 0);
    assert_int_equal(remaining, 0);
    for (size_t i = 0; i < 2; ++i) {
        assert_int_equal(logs[i].count, 1);
        assert_int_equal(logs[i].events, EVIO_READ);
        evio_rx_subscription_free(logs[i].subscription);
    }
    char byte;
    assert_int_equal(read(fds[0], &byte, 1), 1);
    assert_int_equal(byte, 'x');
    assert_true(fcntl(fds[0], F_GETFD) >= 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_observable_free(source);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_rx_poll_pending_cancel_and_fd_reuse)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int fds[2];
    int replacement[2];
    assert_int_equal(pipe(fds), 0);
    assert_int_equal(pipe(replacement), 0);
    int fd = fds[0];
    evio_rx_observable *source = evio_rx_from_poll(fd, EVIO_READ);
    poll_log old = { .fd = fd };
    old.subscription = evio_rx_subscription_new(loop, source, poll_observer(&old), 1);
    evio_rx_subscription_start(old.subscription);
    evio_poll_update(loop);
    evio_feed_fd_event(loop, fd, EVIO_READ);
    assert_true(evio_pending_count(loop) > 0);
    evio_rx_subscription_cancel(old.subscription);
    assert_true(fcntl(fd, F_GETFD) >= 0);
    evio_invoke_pending(loop);
    assert_int_equal(old.count, 0);
    evio_rx_subscription_free(old.subscription);
    close(fd);
    assert_int_equal(dup2(replacement[0], fd), fd);
    poll_log current = { .fd = fd, .consume = true, .cancel = true };
    current.subscription = evio_rx_subscription_new(loop, source, poll_observer(&current), 1);
    evio_rx_subscription_start(current.subscription);
    evio_poll_update(loop);
    assert_int_equal(write(replacement[1], "y", 1), 1);
    evio_poll_wait(loop, 0);
    evio_invoke_pending(loop);
    assert_int_equal(old.count, 0);
    assert_int_equal(current.count, 1);
    assert_int_equal(current.byte, 'y');
    assert_int_equal(current.events, EVIO_READ);
    assert_true(fcntl(fd, F_GETFD) >= 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(current.subscription);
    evio_rx_observable_free(source);
    close(fd);
    close(fds[1]);
    close(replacement[0]);
    close(replacement[1]);
    evio_loop_free(loop);
}

TEST(test_evio_rx_poll_error_is_eio_and_does_not_close_fd)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int fds[2];
    assert_int_equal(pipe(fds), 0);
    evio_rx_observable *source = evio_rx_from_poll(fds[0], EVIO_READ);
    poll_log log = { .fd = fds[0] };
    log.subscription = evio_rx_subscription_new(loop, source, poll_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    evio_feed_fd_error(loop, fds[0]);
    errno = EBUSY;
    evio_invoke_pending(loop);
    assert_int_equal(log.error, EIO);
    assert_int_equal(log.count, 0);
    assert_true(evio_rx_subscription_is_closed(log.subscription));
    assert_true(fcntl(fds[0], F_GETFD) >= 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    close(fds[0]);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_rx_poll_closed_fd_and_invalid_configuration)
{
    assert_null(evio_rx_from_poll(-1, EVIO_READ));
    assert_null(evio_rx_from_poll(0, 0));
    assert_null(evio_rx_from_poll(0, EVIO_READ | EVIO_TIMER));
    assert_null(evio_rx_from_poll(0, EVIO_ERROR));
    assert_int_equal(errno, EINVAL);
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int fds[2];
    assert_int_equal(pipe(fds), 0);
    close(fds[0]);
    evio_rx_observable *source = evio_rx_from_poll(fds[0], EVIO_READ);
    assert_non_null(source);
    poll_log log = { .fd = fds[0] };
    log.subscription = evio_rx_subscription_new(loop, source, poll_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    evio_poll_update(loop);
    evio_invoke_pending(loop);
    assert_int_equal(log.error, EIO);
    assert_int_equal(log.count, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    close(fds[1]);
    evio_loop_free(loop);
}

TEST(test_evio_rx_user_source_invalid_constructor_keeps_capture)
{
    producer_log producer = {0};
    evio_rx_capture capture = { .data = &producer, .destroy = capture_destroy };
    assert_null(evio_rx_observable_new(NULL, producer_source, capture));
    assert_int_equal(errno, EINVAL);
    evio_rx_source invalid = producer_source;
    invalid.prepare = NULL;
    assert_null(evio_rx_observable_new(&int_type, invalid, capture));
    invalid = producer_source;
    invalid.start = NULL;
    assert_null(evio_rx_observable_new(&int_type, invalid, capture));
    assert_int_equal(producer.captures, 0);
}
