#include "test.h"
#include "evio_rx.h"

static const evio_rx_type int_type = {
    .size = sizeof(int), .alignment = alignof(int),
};

typedef struct flat_driver flat_driver;
typedef struct flat_log flat_log;
struct flat_log {
    evio_rx_subscription *subscription;
    evio_loop *loop;
    evio_timer *watchdog;
    flat_driver *outer;
    void (*action)(flat_log *log);
    int values[32];
    size_t count;
    size_t completed;
    size_t depth;
    int error;
};

static void flat_next(void *ctx, const void *value)
{
    flat_log *log = ctx;
    assert_int_equal(log->depth++, 0);
    assert_true(log->count < 32);
    log->values[log->count++] = *(const int *)value;
    if (log->action) {
        log->action(log);
    }
    log->depth--;
}

static void flat_error(void *ctx, int error)
{
    flat_log *log = ctx;
    assert_int_equal(log->depth, 0);
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->error = error;
    if (log->watchdog) {
        evio_timer_stop(log->loop, log->watchdog);
    }
}

static void flat_complete(void *ctx)
{
    flat_log *log = ctx;
    assert_int_equal(log->depth, 0);
    assert_int_equal(log->error, 0);
    assert_int_equal(log->completed, 0);
    assert_true(evio_rx_subscription_is_closed(log->subscription));
    log->completed++;
    if (log->watchdog) {
        evio_timer_stop(log->loop, log->watchdog);
    }
}

static evio_rx_observer flat_observer(flat_log *log)
{
    return (evio_rx_observer) {
        .next = flat_next, .error = flat_error, .complete = flat_complete, .ctx = log,
    };
}

typedef struct flat_state flat_state;
struct flat_driver {
    flat_state *current;
    size_t prepared;
    size_t started;
    size_t stopped;
    size_t destroyed;
    bool last_active;
};

struct flat_state {
    flat_driver *driver;
    evio_rx_emitter *emitter;
    bool started;
    bool stopped;
};

static int flat_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    flat_driver *driver = ctx;
    flat_state *result = evio_calloc(1, sizeof(*result));
    result->driver = driver;
    result->emitter = emitter;
    driver->prepared++;
    *state = result;
    return 0;
}

static void flat_start(evio_rx_emitter *emitter, void *state)
{
    flat_state *producer = state;
    assert_null(producer->driver->current);
    producer->started = true;
    producer->driver->current = producer;
    producer->driver->started++;
}

static void flat_stop(void *state)
{
    flat_state *producer = state;
    assert_true(producer->started);
    assert_false(producer->stopped);
    assert_ptr_equal(producer->driver->current, producer);
    producer->stopped = true;
    producer->driver->current = NULL;
    producer->driver->stopped++;
}

static void flat_destroy(void *state)
{
    flat_state *producer = state;
    assert_true(producer->driver->current != producer);
    if (producer->started) {
        assert_true(producer->stopped);
    }
    producer->driver->destroyed++;
    evio_free(producer);
}

static const evio_rx_source flat_source = {
    .prepare = flat_prepare, .start = flat_start,
    .stop = flat_stop, .destroy = flat_destroy,
};

static void send_next(evio_rx_emitter *emitter, void *state, void *data)
{
    flat_state *producer = state;
    bool active = evio_rx_emitter_next(emitter, data);
    producer->driver->last_active = active;
}

static void publish(flat_driver *driver, int value)
{
    assert_non_null(driver->current);
    evio_rx_emitter *emitter = driver->current->emitter;
    evio_rx_emitter_invoke(emitter, send_next, &value);
}

static void send_complete(evio_rx_emitter *emitter, void *state, void *data)
{
    evio_rx_emitter_complete(emitter);
}

static void finish(flat_driver *driver)
{
    assert_non_null(driver->current);
    evio_rx_emitter *emitter = driver->current->emitter;
    evio_rx_emitter_invoke(emitter, send_complete, NULL);
}

static int empty_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    *state = NULL;
    return 0;
}

static void empty_start(evio_rx_emitter *emitter, void *state)
{
    evio_rx_emitter_complete(emitter);
}

static const evio_rx_source empty_source = {
    .prepare = empty_prepare, .start = empty_start,
};

typedef struct {
    size_t prepared;
    size_t started;
    size_t stopped;
    size_t destroyed;
    size_t fired;
    size_t active;
    size_t peak;
    size_t capture_destroyed;
} timer_stats;

typedef struct {
    timer_stats *stats;
    evio_time after;
    int value;
} timer_capture;

typedef struct {
    const void *self;
    timer_capture *capture;
    evio_loop *loop;
    evio_rx_emitter *emitter;
    evio_timer watcher;
} timer_state;

static void timer_dispatch(evio_rx_emitter *emitter, void *state, void *data)
{
    timer_state *producer = state;
    assert_ptr_equal(producer->self, producer);
    producer->capture->stats->fired++;
    int value = producer->capture->value;
    if (evio_rx_emitter_next(emitter, &value)) {
        evio_rx_emitter_complete(emitter);
    }
    assert_ptr_equal(producer->self, producer);
    assert_int_equal(producer->capture->value, value);
}

static void timer_cb(evio_loop *loop, evio_base *watcher, evio_mask emask)
{
    timer_state *producer = watcher->data;
    evio_rx_emitter *emitter = producer->emitter;
    evio_rx_emitter_invoke(emitter, timer_dispatch, NULL);
}

static int timer_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    timer_capture *capture = ctx;
    timer_state *producer = evio_calloc(1, sizeof(*producer));
    producer->self = producer;
    producer->capture = capture;
    producer->loop = loop;
    producer->emitter = emitter;
    evio_timer_init(&producer->watcher, timer_cb, 0);
    producer->watcher.data = producer;
    capture->stats->prepared++;
    *state = producer;
    return 0;
}

static void timer_start(evio_rx_emitter *emitter, void *state)
{
    timer_state *producer = state;
    timer_stats *stats = producer->capture->stats;
    stats->started++;
    stats->active++;
    if (stats->active > stats->peak) {
        stats->peak = stats->active;
    }
    evio_timer_start(producer->loop, &producer->watcher, producer->capture->after);
}

static void timer_stop(void *state)
{
    timer_state *producer = state;
    timer_stats *stats = producer->capture->stats;
    assert_true(stats->active > 0);
    stats->active--;
    stats->stopped++;
    evio_timer_stop(producer->loop, &producer->watcher);
}

static void timer_destroy(void *state)
{
    timer_state *producer = state;
    assert_int_equal(producer->watcher.active, 0);
    producer->capture->stats->destroyed++;
    evio_free(producer);
}

static void timer_capture_free(void *data)
{
    timer_capture *capture = data;
    capture->stats->capture_destroyed++;
    evio_free(capture);
}

static const evio_rx_source timer_source = {
    .prepare = timer_prepare, .start = timer_start,
    .stop = timer_stop, .destroy = timer_destroy,
};

typedef struct {
    flat_driver *outer;
    timer_stats stats;
    evio_time after[2];
    size_t calls;
    bool reenter;
} timer_project;

static evio_rx_observable *project_timer(void *ctx, const void *value)
{
    timer_project *project = ctx;
    int number = *(const int *)value;
    assert_true(number >= 1 && number <= 2);
    assert_int_equal(project->stats.active, 0);
    project->calls++;
    if (project->reenter && number == 1) {
        publish(project->outer, 2);
    }
    timer_capture *capture = evio_malloc(sizeof(*capture));
    *capture = (timer_capture) {
        .stats = &project->stats,
        .after = project->after[number - 1],
        .value = number,
    };
    return evio_rx_observable_new(&int_type, timer_source,
                                  (evio_rx_capture) { .data = capture, .destroy = timer_capture_free });
}

static void advance(evio_loop *loop, evio_time time)
{
    loop->time = time;
    evio_timer_update(loop);
    evio_invoke_pending(loop);
}

TEST(test_evio_rx_switch_map_timer_cancels_previous_and_waits_after_outer_complete)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    flat_driver outer = {0};
    timer_project project = { .outer = &outer, .after = {5, 5} };
    evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                        (evio_rx_capture) { .data = &outer });
    evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_timer,
        .capture = { .data = &project },
    });
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
    evio_rx_observable_free(source);
    evio_rx_observable_free(switched);
    evio_rx_subscription_start(log.subscription);
    publish(&outer, 1);
    loop->time = 102;
    publish(&outer, 2);
    assert_int_equal(project.calls, 2);
    assert_int_equal(project.stats.started, 2);
    assert_int_equal(project.stats.stopped, 1);
    assert_int_equal(project.stats.destroyed, 1);
    assert_int_equal(project.stats.capture_destroyed, 1);
    assert_int_equal(loop->refcount, 1);
    advance(loop, 105);
    assert_int_equal(log.count, 0);
    finish(&outer);
    assert_int_equal(log.completed, 0);
    advance(loop, 107);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.values[0], 2);
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(project.stats.prepared, 2);
    assert_int_equal(project.stats.stopped, 2);
    assert_int_equal(project.stats.destroyed, 2);
    assert_int_equal(project.stats.capture_destroyed, 2);
    assert_int_equal(project.stats.fired, 1);
    assert_int_equal(project.stats.active, 0);
    assert_int_equal(project.stats.peak, 1);
    assert_int_equal(outer.stopped, 1);
    assert_int_equal(outer.destroyed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_loop_free(loop);
}

static void queue_replacement_and_old_timer(flat_log *log)
{
    if (log->values[log->count - 1] != 99) {
        return;
    }
    publish(log->outer, 2);
    evio_invoke_pending(log->loop);
    assert_int_equal(log->count, 1);
    assert_int_equal(log->error, 0);
}

TEST(test_evio_rx_switch_map_timer_old_queued_next_and_terminal_are_ignored)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    flat_driver outer = {0};
    timer_project project = { .outer = &outer, .after = {5, 5} };
    evio_rx_subject *trigger = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                        (evio_rx_capture) { .data = &outer });
    evio_rx_observable *sources[] = {
        evio_rx_switch_map(source, (evio_rx_project) {
            .type = &int_type,
            .cb = project_timer,
            .capture = { .data = &project },
        }),
        evio_rx_from_subject(trigger),
    };
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    flat_log log = { .loop = loop, .outer = &outer, .action = queue_replacement_and_old_timer };
    log.subscription = evio_rx_subscription_new(loop, merged, flat_observer(&log), 3);
    evio_rx_subscription_start(log.subscription);
    publish(&outer, 1);
    loop->time = 105;
    evio_timer_update(loop);
    assert_int_equal(evio_pending_count(loop), 1);
    int value = 99;
    assert_int_equal(evio_rx_subject_next(trigger, &value), 0);
    assert_int_equal(log.count, 1);
    assert_int_equal(project.stats.fired, 1);
    assert_int_equal(project.stats.stopped, 1);
    assert_int_equal(project.stats.destroyed, 1);
    assert_int_equal(loop->refcount, 1);
    advance(loop, 110);
    int expected[] = {99, 2};
    assert_int_equal(log.count, 2);
    assert_memory_equal(log.values, expected, sizeof(expected));
    finish(&outer);
    evio_rx_subject_complete(trigger);
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(project.stats.fired, 2);
    assert_int_equal(project.stats.stopped, 2);
    assert_int_equal(project.stats.destroyed, 2);
    assert_int_equal(project.stats.capture_destroyed, 2);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(trigger);
    evio_loop_free(loop);
}

static void watchdog_cb(evio_loop *loop, evio_base *watcher, evio_mask emask)
{
    bool *expired = watcher->data;
    *expired = true;
    evio_break(loop, EVIO_BREAK_ALL);
}

TEST(test_evio_rx_switch_map_timer_real_loop_cancels_long_inner_after_projector_reentry)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    flat_driver outer = {0};
    timer_project project = { .outer = &outer, .after = {EVIO_TIME_FROM_SEC(60), 0}, .reenter = true };
    evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                        (evio_rx_capture) { .data = &outer });
    evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_timer,
        .capture = { .data = &project },
    });
    bool expired = false;
    evio_timer watchdog;
    evio_timer_init(&watchdog, watchdog_cb, 0);
    watchdog.data = &expired;
    flat_log log = { .loop = loop, .watchdog = &watchdog };
    log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    publish(&outer, 1);
    assert_int_equal(log.count, 0);
    assert_int_equal(project.calls, 2);
    assert_int_equal(project.stats.started, 2);
    assert_int_equal(project.stats.stopped, 1);
    assert_int_equal(project.stats.destroyed, 1);
    finish(&outer);
    evio_timer_start(loop, &watchdog, EVIO_TIME_FROM_SEC(5));
    evio_run(loop, 0);
    evio_timer_stop(loop, &watchdog);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(switched);
    assert_false(expired);
    assert_int_equal(log.count, 1);
    assert_int_equal(log.values[0], 2);
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(project.stats.prepared, 2);
    assert_int_equal(project.stats.started, 2);
    assert_int_equal(project.stats.stopped, 2);
    assert_int_equal(project.stats.destroyed, 2);
    assert_int_equal(project.stats.capture_destroyed, 2);
    assert_int_equal(project.stats.fired, 1);
    assert_int_equal(project.stats.active, 0);
    assert_int_equal(project.stats.peak, 1);
    assert_int_equal(loop->refcount, 0);
    evio_loop_free(loop);
}

static evio_rx_observable *project_burst(void *ctx, const void *value)
{
    size_t *calls = ctx;
    (*calls)++;
    assert_int_equal(errno, 0);
    int number = *(const int *)value;
    if (!number) {
        return evio_rx_from_array(&int_type, NULL, 0);
    }
    int values[] = {(number * 2) - 1, number * 2};
    return evio_rx_from_array(&int_type, values, 2);
}

static void reenter_outer(flat_log *log)
{
    if (log->values[log->count - 1] == 1) {
        publish(log->outer, 2);
    }
}

TEST(test_evio_rx_switch_map_inner_burst_finishes_before_reentrant_outer)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    flat_driver outer = {0};
    size_t calls = 0;
    evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                        (evio_rx_capture) { .data = &outer });
    evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_burst,
        .capture = { .data = &calls },
    });
    flat_log log = { .outer = &outer, .action = reenter_outer };
    log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    publish(&outer, 1);
    int expected[] = {1, 2, 3, 4};
    assert_int_equal(log.count, 4);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(calls, 2);
    finish(&outer);
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(outer.stopped, 1);
    assert_int_equal(outer.destroyed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(switched);
    evio_loop_free(loop);
}

TEST(test_evio_rx_flatten_restart_repeats_synchronous_run)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int outer[] = {1, 2};
    size_t calls = 0;
    evio_rx_observable *source = evio_rx_from_array(&int_type, outer, 2);
    evio_rx_project project = { .type = &int_type, .cb = project_burst, .capture = { .data = &calls } };
    evio_rx_observable *operators[] = {
        evio_rx_switch_map(source, project),
        evio_rx_concat_map(source, project, 2),
        evio_rx_merge_map(source, project, 1, 2),
    };
    for (size_t i = 0; i < 3; ++i) {
        flat_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, operators[i], flat_observer(&log), 1);
        assert_non_null(log.subscription);
        for (size_t run = 0; run < 2; ++run) {
            log = (flat_log) { .subscription = log.subscription };
            errno = 0;
            if (run) {
                evio_rx_subscription_restart(log.subscription);
            } else {
                evio_rx_subscription_start(log.subscription);
            }
            int expected[] = {1, 2, 3, 4};
            assert_int_equal(log.count, 4);
            assert_memory_equal(log.values, expected, sizeof(expected));
            assert_int_equal(log.completed, 1);
        }
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(operators[i]);
    }
    assert_int_equal(calls, 12);
    evio_rx_observable_free(source);
    evio_loop_free(loop);
}

typedef struct {
    evio_rx_observable *inner;
    size_t *calls;
    size_t *destroyed;
} ref_project;

static evio_rx_observable *project_ref(void *ctx, const void *value)
{
    ref_project *project = ctx;
    (*project->calls)++;
    return evio_rx_observable_ref(project->inner);
}

static void ref_project_free(void *data)
{
    ref_project *project = data;
    evio_rx_observable_free(project->inner);
    (*project->destroyed)++;
    evio_free(project);
}

TEST(test_evio_rx_switch_map_reuses_owned_inner_and_independent_runs_with_small_root_fifo)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int outer[] = {1, 2};
    int inner[] = {10, 20, 30, 40};
    size_t calls = 0;
    size_t destroyed = 0;
    ref_project *project = evio_malloc(sizeof(*project));
    *project = (ref_project) {
        .inner = evio_rx_from_array(&int_type, inner, 4),
        .calls = &calls,
        .destroyed = &destroyed,
    };
    evio_rx_observable *source = evio_rx_from_array(&int_type, outer, 2);
    evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_ref,
        .capture = { .data = project, .destroy = ref_project_free },
    });
    flat_log logs[2] = {0};
    for (size_t i = 0; i < 2; ++i) {
        logs[i].subscription = evio_rx_subscription_new(loop, switched, flat_observer(&logs[i]), 1);
    }
    evio_rx_observable_free(source);
    evio_rx_observable_free(switched);
    evio_rx_subscription_start(logs[0].subscription);
    assert_int_equal(destroyed, 0);
    evio_rx_subscription_start(logs[1].subscription);
    int expected[] = {10, 20, 30, 40, 10, 20, 30, 40};
    for (size_t i = 0; i < 2; ++i) {
        assert_int_equal(logs[i].count, 8);
        assert_memory_equal(logs[i].values, expected, sizeof(expected));
        assert_int_equal(logs[i].completed, 1);
        assert_int_equal(logs[i].error, 0);
        evio_rx_subscription_free(logs[i].subscription);
    }
    assert_int_equal(calls, 4);
    assert_int_equal(destroyed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_loop_free(loop);
}

static int sum_step(void *ctx, void *out, const void *accumulator, const void *value)
{
    *(int *)out = *(const int *)accumulator + *(const int *)value;
    return 0;
}

static evio_rx_observable *project_same(void *ctx, const void *value)
{
    return evio_rx_observable_ref(ctx);
}

TEST(test_evio_rx_switch_map_replaced_same_inner_restarts_from_seed)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    flat_driver outer = {0};
    flat_driver inner = {0};
    int seed = 100;
    evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                        (evio_rx_capture) { .data = &outer });
    evio_rx_observable *producer = evio_rx_observable_new(&int_type, flat_source,
                                                          (evio_rx_capture) { .data = &inner });
    evio_rx_observable *scanned = evio_rx_scan(producer, &int_type, &seed, sum_step,
                                               (evio_rx_capture) {0});
    evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_same,
        .capture = { .data = scanned },
    });
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    publish(&outer, 1);
    publish(&inner, 1);
    publish(&inner, 2);
    publish(&outer, 2);
    assert_int_equal(inner.stopped, 1);
    assert_int_equal(inner.destroyed, 1);
    assert_int_equal(inner.started, 2);
    publish(&inner, 5);
    finish(&outer);
    finish(&inner);
    int expected[] = {101, 103, 105};
    assert_int_equal(log.count, 3);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(inner.prepared, 2);
    assert_int_equal(inner.destroyed, 2);
    assert_int_equal(outer.destroyed, 1);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(switched);
    evio_rx_observable_free(scanned);
    evio_rx_observable_free(producer);
    evio_rx_observable_free(source);
    assert_int_equal(loop->refcount, 0);
    evio_loop_free(loop);
}

static evio_rx_observable *project_driver_once(void *ctx, const void *value)
{
    if (*(const int *)value > 1) {
        errno = EDOM;
        return NULL;
    }
    return evio_rx_observable_new(&int_type, flat_source, (evio_rx_capture) { .data = ctx });
}

TEST(test_evio_rx_switch_map_replaced_same_inner_releases_adopted_inner_nodes)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    flat_driver outer = {0};
    flat_driver leaf = {0};
    int one = 1;
    evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                        (evio_rx_capture) { .data = &outer });
    evio_rx_observable *head = evio_rx_from_array(&int_type, &one, 1);
    evio_rx_observable *nested = evio_rx_switch_map(head, (evio_rx_project) {
        .type = &int_type,
        .cb = project_driver_once,
        .capture = { .data = &leaf },
    });
    evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_same,
        .capture = { .data = nested },
    });
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    publish(&outer, 1);
    publish(&leaf, 10);
    publish(&outer, 2);
    assert_int_equal(leaf.started, 2);
    assert_int_equal(leaf.stopped, 1);
    assert_int_equal(leaf.destroyed, 1);
    publish(&leaf, 20);
    finish(&outer);
    finish(&leaf);
    int expected[] = {10, 20};
    assert_int_equal(log.count, 2);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(leaf.prepared, 2);
    assert_int_equal(leaf.destroyed, 2);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(switched);
    evio_rx_observable_free(nested);
    evio_rx_observable_free(head);
    evio_rx_observable_free(source);
    assert_int_equal(loop->refcount, 0);
    evio_loop_free(loop);
}

static void cancel_flat(flat_log *log)
{
    evio_rx_subscription_cancel(log->subscription);
}

static void free_flat(flat_log *log)
{
    evio_rx_subscription_free(log->subscription);
    log->subscription = NULL;
}

TEST(test_evio_rx_switch_map_cancel_and_free_first_next_stop_inner_and_outer_bursts)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2};
    for (size_t i = 0; i < 2; ++i) {
        size_t calls = 0;
        evio_rx_observable *source = evio_rx_from_array(&int_type, values, 2);
        evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
            .type = &int_type,
            .cb = project_burst,
            .capture = { .data = &calls },
        });
        flat_log log = { .action = i ? free_flat : cancel_flat };
        log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
        evio_rx_observable_free(source);
        evio_rx_observable_free(switched);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, 1);
        assert_int_equal(log.values[0], 1);
        assert_int_equal(log.completed, 0);
        assert_int_equal(log.error, 0);
        assert_int_equal(calls, 1);
        if (i) {
            assert_null(log.subscription);
        } else {
            assert_true(evio_rx_subscription_is_closed(log.subscription));
            evio_rx_subscription_free(log.subscription);
        }
        assert_int_equal(loop->refcount, 0);
    }
    evio_loop_free(loop);
}

static void count_capture(void *data)
{
    size_t *destroyed = data;
    (*destroyed)++;
}

TEST(test_evio_rx_switch_map_empty_inner_and_outer_and_invalid_configuration)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        int values[] = {0, 1, 2};
        size_t calls = 0;
        size_t destroyed = 0;
        evio_rx_observable *source = evio_rx_from_array(&int_type, values, i ? 3 : 0);
        evio_rx_project invalid = {
            .type = &int_type,
            .cb = project_burst,
            .capture = { .data = &destroyed, .destroy = count_capture },
        };
        assert_null(evio_rx_switch_map(NULL, invalid));
        assert_int_equal(errno, EINVAL);
        invalid.cb = NULL;
        assert_null(evio_rx_switch_map(source, invalid));
        assert_int_equal(errno, EINVAL);
        invalid.cb = project_burst;
        invalid.type = NULL;
        assert_null(evio_rx_switch_map(source, invalid));
        assert_int_equal(errno, EINVAL);
        const evio_rx_type bad_type = { .size = 3, .alignment = 2 };
        invalid.type = &bad_type;
        assert_null(evio_rx_switch_map(source, invalid));
        assert_int_equal(errno, EINVAL);
        assert_int_equal(destroyed, 0);
        evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
            .type = &int_type,
            .cb = project_burst,
            .capture = { .data = &calls },
        });
        flat_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        int expected[] = {1, 2, 3, 4};
        assert_int_equal(log.count, i ? 4 : 0);
        if (i) {
            assert_memory_equal(log.values, expected, sizeof(expected));
        }
        assert_int_equal(calls, i ? 3 : 0);
        assert_int_equal(log.completed, 1);
        assert_int_equal(log.error, 0);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(switched);
    }
    evio_loop_free(loop);
}

typedef struct {
    alignas(128) const void *self;
    int *number;
    unsigned char bytes[4096];
} large_value;

typedef struct {
    size_t copied;
    size_t destroyed;
    size_t live;
    size_t mapped;
    int last_destroyed;
} large_stats;

static int large_copy(void *ctx, void *dst, const void *src)
{
    large_stats *stats = ctx;
    const large_value *value = src;
    large_value *out = dst;
    assert_ptr_equal(value->self, value);
    assert_int_equal((uintptr_t)out % alignof(large_value), 0);
    out->self = out;
    out->number = evio_malloc(sizeof(*out->number));
    *out->number = *value->number;
    memcpy(out->bytes, value->bytes, sizeof(out->bytes));
    stats->copied++;
    stats->live++;
    return 0;
}

static void large_destroy(void *ctx, void *value)
{
    large_stats *stats = ctx;
    large_value *number = value;
    assert_ptr_equal(number->self, number);
    assert_int_equal((uintptr_t)number % alignof(large_value), 0);
    assert_int_equal(number->bytes[0], 17);
    assert_int_equal(number->bytes[sizeof(number->bytes) - 1], 17);
    assert_true(stats->live > 0);
    stats->last_destroyed = *number->number;
    evio_free(number->number);
    stats->live--;
    stats->destroyed++;
}

static int large_to_int(void *ctx, void *out, const void *value)
{
    large_stats *stats = ctx;
    const large_value *number = value;
    assert_ptr_equal(number->self, number);
    assert_int_equal((uintptr_t)number % alignof(large_value), 0);
    assert_int_equal(number->bytes[0], 17);
    stats->mapped++;
    *(int *)out = *number->number;
    return 0;
}

typedef struct {
    evio_rx_observable *source;
    flat_driver *driver;
    large_stats *stats;
    size_t calls;
} large_project;

static evio_rx_observable *project_large(void *ctx, const void *value)
{
    large_project *project = ctx;
    assert_null(project->driver->current);
    project->calls++;
    return evio_rx_map(project->source, &int_type, large_to_int,
                       (evio_rx_capture) { .data = project->stats });
}

typedef struct {
    flat_log log;
    flat_driver *inner;
} large_log;

static void replace_before_large_input(flat_log *base)
{
    large_log *log = (large_log *)base;
    if (base->values[base->count - 1] != 99) {
        return;
    }
    publish(base->outer, 2);
    int number = 7;
    large_value value = { .self = &value, .number = &number };
    memset(value.bytes, 17, sizeof(value.bytes));
    assert_non_null(log->inner->current);
    evio_rx_emitter *emitter = log->inner->current->emitter;
    evio_rx_emitter_invoke(emitter, send_next, &value);
    number = 99;
    memset(value.bytes, 42, sizeof(value.bytes));
}

TEST(test_evio_rx_switch_map_large_dynamic_input_destroys_stale_owned_queue_value)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    flat_driver outer = {0};
    flat_driver inner = {0};
    large_stats stats = {0};
    const evio_rx_type type = {
        .size = sizeof(large_value), .alignment = alignof(large_value),
        .copy = large_copy, .destroy = large_destroy, .ctx = &stats,
    };
    evio_rx_observable *input = evio_rx_observable_new(&type, flat_source,
                                                       (evio_rx_capture) { .data = &inner });
    large_project project = { .source = input, .driver = &inner, .stats = &stats };
    evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                        (evio_rx_capture) { .data = &outer });
    evio_rx_subject *trigger = evio_rx_subject_new(loop, &int_type, 1);
    evio_rx_observable *sources[] = {
        evio_rx_switch_map(source, (evio_rx_project) {
            .type = &int_type,
            .cb = project_large,
            .capture = { .data = &project },
        }),
        evio_rx_from_subject(trigger),
    };
    evio_rx_observable *merged = evio_rx_merge(sources, 2);
    large_log log = { .log = { .outer = &outer, .action = replace_before_large_input }, .inner = &inner };
    log.log.subscription = evio_rx_subscription_new(loop, merged, flat_observer(&log.log), 2);
    evio_rx_subscription_start(log.log.subscription);
    publish(&outer, 1);
    int control = 99;
    assert_int_equal(evio_rx_subject_next(trigger, &control), 0);
    assert_int_equal(log.log.count, 1);
    assert_int_equal(log.log.values[0], 99);
    assert_int_equal(project.calls, 2);
    assert_int_equal(inner.stopped, 1);
    assert_int_equal(inner.destroyed, 1);
    assert_int_equal(stats.copied, 1);
    assert_int_equal(stats.destroyed, 1);
    assert_int_equal(stats.last_destroyed, 7);
    assert_int_equal(stats.live, 0);
    assert_int_equal(stats.mapped, 0);
    int number = 8;
    large_value value = { .self = &value, .number = &number };
    memset(value.bytes, 17, sizeof(value.bytes));
    assert_non_null(inner.current);
    evio_rx_emitter *emitter = inner.current->emitter;
    evio_rx_emitter_invoke(emitter, send_next, &value);
    int expected[] = {99, 8};
    assert_int_equal(log.log.count, 2);
    assert_memory_equal(log.log.values, expected, sizeof(expected));
    assert_int_equal(stats.mapped, 1);
    assert_int_equal(stats.copied, 1);
    finish(&outer);
    finish(&inner);
    evio_rx_subject_complete(trigger);
    assert_int_equal(log.log.completed, 1);
    assert_int_equal(log.log.error, 0);
    assert_int_equal(inner.prepared, 2);
    assert_int_equal(inner.started, 2);
    assert_int_equal(inner.stopped, 2);
    assert_int_equal(inner.destroyed, 2);
    assert_int_equal(stats.live, 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(input);
    evio_rx_observable_free(merged);
    evio_rx_observable_free(sources[0]);
    evio_rx_observable_free(sources[1]);
    evio_rx_subject_free(trigger);
    evio_loop_free(loop);
}

typedef enum {
    FAILURE_NULL,
    FAILURE_TYPE,
    FAILURE_LOOP,
    FAILURE_PREPARE,
    FAILURE_INNER,
    FAILURE_CANCEL,
    FAILURE_FREE,
} failure_mode;

typedef struct {
    failure_mode mode;
    flat_log *log;
    evio_rx_subject *other;
    int error;
    size_t calls;
    size_t prepared;
    size_t started;
    size_t stopped;
    size_t destroyed;
    size_t released;
    size_t mapped;
} failure_project;

typedef struct {
    failure_project *project;
} failure_capture;

static int failure_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    failure_capture *capture = ctx;
    capture->project->prepared++;
    if (capture->project->mode == FAILURE_PREPARE) {
        return EACCES;
    }
    *state = capture;
    return 0;
}

static void failure_start(evio_rx_emitter *emitter, void *state)
{
    failure_capture *capture = state;
    capture->project->started++;
    evio_rx_emitter_error(emitter, EIO);
}

static void failure_stop(void *state)
{
    failure_capture *capture = state;
    capture->project->stopped++;
}

static void failure_destroy(void *state)
{
    failure_capture *capture = state;
    capture->project->destroyed++;
}

static void failure_capture_free(void *data)
{
    failure_capture *capture = data;
    capture->project->released++;
    evio_free(capture);
    errno = ERANGE;
}

static int failure_map(void *ctx, void *out, const void *value)
{
    failure_capture *capture = ctx;
    capture->project->mapped++;
    *(int *)out = *(const int *)value;
    return 0;
}

static const evio_rx_source failure_source = {
    .prepare = failure_prepare, .start = failure_start,
    .stop = failure_stop, .destroy = failure_destroy,
};

static const evio_rx_type other_int_type = {
    .size = sizeof(int), .alignment = alignof(int),
};

static evio_rx_observable *project_failure(void *ctx, const void *value)
{
    failure_project *project = ctx;
    assert_int_equal(errno, 0);
    project->calls++;
    if (project->mode == FAILURE_NULL) {
        errno = project->error;
        return NULL;
    }
    failure_capture *capture = evio_malloc(sizeof(*capture));
    capture->project = project;
    evio_rx_observable *result;
    if (project->mode == FAILURE_LOOP) {
        evio_rx_observable *source = evio_rx_from_subject(project->other);
        result = evio_rx_map(source, &int_type, failure_map,
                             (evio_rx_capture) { .data = capture, .destroy = failure_capture_free });
        evio_rx_observable_free(source);
    } else {
        const evio_rx_type *type = project->mode == FAILURE_TYPE ? &other_int_type : &int_type;
        result = evio_rx_observable_new(type, failure_source,
                                        (evio_rx_capture) { .data = capture, .destroy = failure_capture_free });
    }
    assert_non_null(result);
    if (project->mode == FAILURE_CANCEL) {
        evio_rx_subscription_cancel(project->log->subscription);
    } else if (project->mode == FAILURE_FREE) {
        evio_rx_subscription_free(project->log->subscription);
        project->log->subscription = NULL;
    }
    errno = ERANGE;
    return result;
}

TEST(test_evio_rx_switch_map_projector_null_errno_is_reset_and_normalized)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int errors[] = {EIO, 0, -EIO};
    int values[] = {1, 2};
    for (size_t i = 0; i < 3; ++i) {
        failure_project project = { .mode = FAILURE_NULL, .error = errors[i] };
        evio_rx_observable *source = evio_rx_from_array(&int_type, values, 2);
        evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
            .type = &int_type,
            .cb = project_failure,
            .capture = { .data = &project },
        });
        flat_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
        errno = ENOTTY;
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(project.calls, 1);
        assert_int_equal(project.released, 0);
        assert_int_equal(log.count, 0);
        assert_int_equal(log.completed, 0);
        assert_int_equal(log.error, i ? EINVAL : EIO);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(switched);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_switch_map_projector_null_releases_previous_inner)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    flat_driver inner = {0};
    int values[] = {1, 2};
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 2);
    evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_driver_once,
        .capture = { .data = &inner },
    });
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, EDOM);
    assert_int_equal(inner.started, 1);
    assert_int_equal(inner.stopped, 1);
    assert_int_equal(inner.destroyed, 1);
    assert_null(inner.current);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(switched);
    evio_loop_free(loop);
}

TEST(test_evio_rx_flatten_returned_type_loop_prepare_and_sync_inner_errors_release_reference)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    evio_loop *other = evio_loop_new(EVIO_FLAG_NONE);
    evio_rx_subject *subject = evio_rx_subject_new(other, &int_type, 1);
    failure_mode modes[] = {FAILURE_TYPE, FAILURE_LOOP, FAILURE_PREPARE, FAILURE_INNER};
    int errors[] = {EINVAL, EINVAL, EACCES, EIO};
    int values[] = {1, 2};
    for (size_t k = 0; k < 12; ++k) {
        size_t i = k / 3;
        failure_project project = { .mode = modes[i], .other = subject };
        evio_rx_observable *source = evio_rx_from_array(&int_type, values, 2);
        evio_rx_project projector = {
            .type = &int_type,
            .cb = project_failure,
            .capture = { .data = &project },
        };
        evio_rx_observable *operators[] = {
            evio_rx_switch_map(source, projector),
            evio_rx_concat_map(source, projector, 2),
            evio_rx_merge_map(source, projector, 1, 2),
        };
        evio_rx_observable *flattened = operators[k % 3];
        evio_rx_observable_free(operators[(k + 1) % 3]);
        evio_rx_observable_free(operators[(k + 2) % 3]);
        flat_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, flattened, flat_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(log.count, 0);
        assert_int_equal(log.completed, 0);
        assert_int_equal(log.error, errors[i]);
        assert_int_equal(project.calls, 1);
        assert_int_equal(project.released, 1);
        assert_int_equal(project.prepared, i >= 2 ? 1 : 0);
        assert_int_equal(project.started, i == 3 ? 1 : 0);
        assert_int_equal(project.stopped, i == 3 ? 1 : 0);
        assert_int_equal(project.destroyed, i == 3 ? 1 : 0);
        assert_int_equal(project.mapped, 0);
        assert_int_equal(loop->refcount, 0);
        assert_int_equal(other->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(flattened);
    }
    evio_rx_subject_free(subject);
    evio_loop_free(other);
    evio_loop_free(loop);
}

TEST(test_evio_rx_switch_map_cancel_and_free_projector_release_unstarted_returned_reference)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        flat_driver outer = {0};
        flat_log log = {0};
        failure_project project = { .mode = i ? FAILURE_FREE : FAILURE_CANCEL, .log = &log };
        evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                            (evio_rx_capture) { .data = &outer });
        evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
            .type = &int_type,
            .cb = project_failure,
            .capture = { .data = &project },
        });
        log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
        evio_rx_observable_free(source);
        evio_rx_observable_free(switched);
        evio_rx_subscription_start(log.subscription);
        publish(&outer, 1);
        assert_int_equal(log.count, 0);
        assert_int_equal(log.completed, 0);
        assert_int_equal(log.error, 0);
        assert_int_equal(project.calls, 1);
        assert_int_equal(project.released, 1);
        assert_int_equal(project.prepared, 0);
        assert_int_equal(project.started, 0);
        assert_int_equal(project.destroyed, 0);
        assert_false(outer.last_active);
        assert_int_equal(outer.stopped, 1);
        assert_int_equal(outer.destroyed, 1);
        assert_int_equal(loop->refcount, 0);
        if (i) {
            assert_null(log.subscription);
        } else {
            assert_true(evio_rx_subscription_is_closed(log.subscription));
            evio_rx_subscription_free(log.subscription);
        }
    }
    evio_loop_free(loop);
}

static void send_error(evio_rx_emitter *emitter, void *state, void *data)
{
    evio_rx_emitter_error(emitter, *(int *)data);
}

static void fail_producer(flat_driver *driver, int error)
{
    assert_non_null(driver->current);
    evio_rx_emitter *emitter = driver->current->emitter;
    evio_rx_emitter_invoke(emitter, send_error, &error);
}

TEST(test_evio_rx_switch_map_outer_error_cancels_pending_inner_timer)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    loop->time = 100;
    flat_driver outer = {0};
    timer_project project = { .outer = &outer, .after = {5, 5} };
    evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                        (evio_rx_capture) { .data = &outer });
    evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_timer,
        .capture = { .data = &project },
    });
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    publish(&outer, 1);
    loop->time = 105;
    evio_timer_update(loop);
    assert_int_equal(evio_pending_count(loop), 1);
    fail_producer(&outer, EIO);
    evio_invoke_pending(loop);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, EIO);
    assert_int_equal(project.stats.started, 1);
    assert_int_equal(project.stats.stopped, 1);
    assert_int_equal(project.stats.destroyed, 1);
    assert_int_equal(project.stats.capture_destroyed, 1);
    assert_int_equal(project.stats.fired, 0);
    assert_int_equal(project.stats.active, 0);
    assert_int_equal(outer.stopped, 1);
    assert_int_equal(outer.destroyed, 1);
    assert_int_equal(evio_pending_count(loop), 0);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(switched);
    evio_loop_free(loop);
}

static evio_rx_observable *project_driver(void *ctx, const void *value)
{
    return evio_rx_observable_new(&int_type, flat_source, (evio_rx_capture) { .data = ctx });
}

TEST(test_evio_rx_switch_map_inner_error_cancels_outer_and_both_producers)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    flat_driver outer = {0};
    flat_driver inner = {0};
    evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                        (evio_rx_capture) { .data = &outer });
    evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_driver,
        .capture = { .data = &inner },
    });
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    publish(&outer, 1);
    fail_producer(&inner, EIO);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, EIO);
    assert_null(outer.current);
    assert_null(inner.current);
    assert_int_equal(outer.stopped, 1);
    assert_int_equal(outer.destroyed, 1);
    assert_int_equal(inner.stopped, 1);
    assert_int_equal(inner.destroyed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(switched);
    evio_loop_free(loop);
}

typedef struct {
    flat_driver *first;
    size_t calls;
    size_t created;
    size_t destroyed;
    size_t depth;
    size_t peak;
    int fail_at;
    int inner_error_at;
} sequence_project;

typedef struct {
    sequence_project *project;
    int value;
} sequence_capture;

static int sequence_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state)
{
    *state = ctx;
    return 0;
}

static void sequence_start(evio_rx_emitter *emitter, void *state)
{
    const sequence_capture *capture = state;
    sequence_project *project = capture->project;
    project->depth++;
    if (project->depth > project->peak) {
        project->peak = project->depth;
    }
    if (capture->value == project->inner_error_at) {
        evio_rx_emitter_error(emitter, EIO);
    } else if (evio_rx_emitter_next(emitter, &capture->value)) {
        evio_rx_emitter_complete(emitter);
    }
    project->depth--;
}

static void sequence_capture_free(void *data)
{
    sequence_capture *capture = data;
    capture->project->destroyed++;
    evio_free(capture);
}

static const evio_rx_source sequence_source = {
    .prepare = sequence_prepare, .start = sequence_start,
};

static evio_rx_observable *project_sequence(void *ctx, const void *value)
{
    sequence_project *project = ctx;
    int number = *(const int *)value;
    project->calls++;
    assert_int_equal(number, project->calls);
    if (number == project->fail_at) {
        errno = EIO;
        return NULL;
    }
    if (number == 1) {
        return evio_rx_observable_new(&int_type, flat_source,
                                      (evio_rx_capture) { .data = project->first });
    }
    sequence_capture *capture = evio_malloc(sizeof(*capture));
    *capture = (sequence_capture) { .project = project, .value = number };
    project->created++;
    return evio_rx_observable_new(&int_type, sequence_source,
                                  (evio_rx_capture) { .data = capture, .destroy = sequence_capture_free });
}

TEST(test_evio_rx_concat_map_slow_first_defers_projector_and_drains_in_order_after_outer_complete)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3};
    flat_driver first = {0};
    sequence_project project = { .first = &first };
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *concatenated = evio_rx_concat_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_sequence,
        .capture = { .data = &project },
    }, 2);
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, concatenated, flat_observer(&log), 1);
    evio_rx_observable_free(source);
    evio_rx_observable_free(concatenated);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(project.calls, 1);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    publish(&first, 1);
    assert_int_equal(log.count, 1);
    assert_int_equal(project.calls, 1);
    finish(&first);
    int expected[] = {1, 2, 3};
    assert_int_equal(log.count, 3);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(project.calls, 3);
    assert_int_equal(project.created, 2);
    assert_int_equal(project.destroyed, 2);
    assert_int_equal(project.depth, 0);
    assert_int_equal(project.peak, 1);
    assert_int_equal(first.stopped, 1);
    assert_int_equal(first.destroyed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_loop_free(loop);
}

typedef struct {
    size_t count;
    uint64_t checksum;
    size_t completed;
    int error;
} sequence_log;

static void sequence_next(void *ctx, const void *value)
{
    sequence_log *log = ctx;
    int number = *(const int *)value;
    assert_int_equal(number, log->count + 1);
    log->count++;
    log->checksum += number;
}

static void sequence_complete(void *ctx)
{
    sequence_log *log = ctx;
    log->completed++;
}

static void sequence_error(void *ctx, int error)
{
    sequence_log *log = ctx;
    log->error = error;
}

TEST(test_evio_rx_concat_map_long_queued_sync_inner_starts_do_not_nest)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    const size_t count = 1024;
    int *values = evio_malloc(count * sizeof(*values));
    for (size_t i = 0; i < count; ++i) {
        values[i] = i + 1;
    }
    flat_driver first = {0};
    sequence_project project = { .first = &first };
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, count);
    evio_free(values);
    evio_rx_observable *concatenated = evio_rx_concat_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_sequence,
        .capture = { .data = &project },
    }, count - 1);
    sequence_log log = {0};
    evio_rx_subscription *subscription = evio_rx_subscription_new(loop, concatenated, (evio_rx_observer) {
        .next = sequence_next,
        .complete = sequence_complete,
        .error = sequence_error,
        .ctx = &log,
    }, 1);
    evio_rx_subscription_start(subscription);
    assert_int_equal(project.calls, 1);
    publish(&first, 1);
    finish(&first);
    assert_int_equal(log.count, 1024);
    assert_int_equal(log.checksum, 524800);
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(project.calls, 1024);
    assert_int_equal(project.created, 1023);
    assert_int_equal(project.destroyed, 1023);
    assert_int_equal(project.depth, 0);
    assert_int_equal(project.peak, 1);
    assert_int_equal(first.stopped, 1);
    assert_int_equal(first.destroyed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(concatenated);
    evio_loop_free(loop);
}

TEST(test_evio_rx_concat_map_waiting_projector_and_inner_errors_discard_tail)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3};
    for (size_t i = 0; i < 2; ++i) {
        flat_driver first = {0};
        sequence_project project = { .first = &first, .fail_at = i ? 0 : 2, .inner_error_at = i ? 2 : 0 };
        evio_rx_observable *source = evio_rx_from_array(&int_type, values, 3);
        evio_rx_observable *concatenated = evio_rx_concat_map(source, (evio_rx_project) {
            .type = &int_type,
            .cb = project_sequence,
            .capture = { .data = &project },
        }, 2);
        flat_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, concatenated, flat_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        assert_int_equal(project.calls, 1);
        publish(&first, 1);
        finish(&first);
        assert_int_equal(log.count, 1);
        assert_int_equal(log.values[0], 1);
        assert_int_equal(log.error, EIO);
        assert_int_equal(log.completed, 0);
        assert_int_equal(project.calls, 2);
        assert_int_equal(project.created, i ? 1 : 0);
        assert_int_equal(project.destroyed, i ? 1 : 0);
        assert_int_equal(first.stopped, 1);
        assert_int_equal(first.destroyed, 1);
        assert_int_equal(loop->refcount, 0);
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(concatenated);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_concat_map_sync_and_empty_inner_do_not_fill_root_fifo)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {0, 1, 2};
    size_t calls = 0;
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 3);
    evio_rx_observable *concatenated = evio_rx_concat_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_burst,
        .capture = { .data = &calls },
    }, 1);
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, concatenated, flat_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    int expected[] = {1, 2, 3, 4};
    assert_int_equal(log.count, 4);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(calls, 3);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(concatenated);
    evio_loop_free(loop);
}

typedef struct {
    large_stats stats;
    evio_rx_subscription *subscription;
    size_t calls;
    size_t fail_at;
    bool cancel_copy;
} queued_values;

static int queued_copy(void *ctx, void *out, const void *value)
{
    queued_values *values = ctx;
    values->calls++;
    if (values->calls == values->fail_at) {
        return EIO;
    }
    if (values->cancel_copy) {
        evio_rx_subscription_cancel(values->subscription);
    }
    return large_copy(&values->stats, out, value);
}

static void queued_destroy(void *ctx, void *value)
{
    queued_values *values = ctx;
    large_destroy(&values->stats, value);
}

static evio_rx_observable *project_owned_sequence(void *ctx, const void *value)
{
    const large_value *number = value;
    assert_ptr_equal(number->self, number);
    assert_int_equal((uintptr_t)number % alignof(large_value), 0);
    assert_int_equal(number->bytes[0], 17);
    return project_sequence(ctx, number->number);
}

typedef struct {
    flat_driver outer;
    flat_driver first;
    sequence_project project;
    queued_values values;
    evio_rx_type type;
    flat_log log;
} queued_fixture;

static void queued_init(queued_fixture *fixture, evio_loop *loop, size_t capacity)
{
    fixture->project.first = &fixture->first;
    fixture->type = (evio_rx_type) {
        .size = sizeof(large_value), .alignment = alignof(large_value),
        .copy = queued_copy, .destroy = queued_destroy, .ctx = &fixture->values,
    };
    evio_rx_observable *source = evio_rx_observable_new(&fixture->type, flat_source,
                                                        (evio_rx_capture) { .data = &fixture->outer });
    evio_rx_observable *concatenated = evio_rx_concat_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_owned_sequence,
        .capture = { .data = &fixture->project },
    }, capacity);
    fixture->log.loop = loop;
    fixture->log.subscription = evio_rx_subscription_new(loop, concatenated, flat_observer(&fixture->log), 1);
    assert_non_null(fixture->log.subscription);
    fixture->values.subscription = fixture->log.subscription;
    evio_rx_observable_free(source);
    evio_rx_observable_free(concatenated);
}

static void publish_owned(flat_driver *driver, int number)
{
    large_value value = { .self = &value, .number = &number };
    memset(value.bytes, 17, sizeof(value.bytes));
    assert_non_null(driver->current);
    evio_rx_emitter *emitter = driver->current->emitter;
    evio_rx_emitter_invoke(emitter, send_next, &value);
}

static void assert_queued_stopped(const queued_fixture *fixture)
{
    assert_null(fixture->outer.current);
    assert_null(fixture->first.current);
    assert_int_equal(fixture->outer.stopped, 1);
    assert_int_equal(fixture->outer.destroyed, 1);
    assert_int_equal(fixture->first.stopped, 1);
    assert_int_equal(fixture->first.destroyed, 1);
    assert_int_equal(fixture->project.calls, 1);
    assert_int_equal(fixture->values.stats.live, 0);
    assert_int_equal(fixture->values.stats.destroyed, fixture->values.stats.copied);
    assert_int_equal(fixture->log.loop->refcount, 0);
}

TEST(test_evio_rx_concat_map_owned_outer_values_survive_producer_until_projected)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    queued_fixture fixture = {0};
    queued_init(&fixture, loop, 2);
    evio_rx_subscription_start(fixture.log.subscription);
    publish_owned(&fixture.outer, 1);
    publish_owned(&fixture.outer, 2);
    publish_owned(&fixture.outer, 3);
    assert_int_equal(fixture.project.calls, 1);
    assert_int_equal(fixture.values.stats.copied, 2);
    assert_int_equal(fixture.values.stats.live, 2);
    finish(&fixture.outer);
    assert_int_equal(fixture.log.completed, 0);
    publish(&fixture.first, 1);
    finish(&fixture.first);
    int expected[] = {1, 2, 3};
    assert_int_equal(fixture.log.count, 3);
    assert_memory_equal(fixture.log.values, expected, sizeof(expected));
    assert_int_equal(fixture.log.completed, 1);
    assert_int_equal(fixture.log.error, 0);
    assert_int_equal(fixture.project.calls, 3);
    assert_int_equal(fixture.project.created, 2);
    assert_int_equal(fixture.project.destroyed, 2);
    assert_int_equal(fixture.values.stats.copied, 2);
    assert_int_equal(fixture.values.stats.destroyed, 2);
    assert_int_equal(fixture.values.stats.live, 0);
    assert_int_equal(fixture.outer.stopped, 1);
    assert_int_equal(fixture.outer.destroyed, 1);
    assert_int_equal(fixture.first.stopped, 1);
    assert_int_equal(fixture.first.destroyed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(fixture.log.subscription);
    evio_loop_free(loop);
}

TEST(test_evio_rx_concat_map_capacity_overflow_discards_owned_waiting_values)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    queued_fixture fixture = {0};
    queued_init(&fixture, loop, 1);
    evio_rx_subscription_start(fixture.log.subscription);
    publish_owned(&fixture.outer, 1);
    publish_owned(&fixture.outer, 2);
    assert_int_equal(fixture.values.stats.live, 1);
    publish_owned(&fixture.outer, 3);
    assert_int_equal(fixture.log.count, 0);
    assert_int_equal(fixture.log.completed, 0);
    assert_int_equal(fixture.log.error, EOVERFLOW);
    assert_int_equal(fixture.values.calls, 1);
    assert_int_equal(fixture.values.stats.copied, 1);
    assert_queued_stopped(&fixture);
    evio_rx_subscription_free(fixture.log.subscription);
    evio_loop_free(loop);
}

TEST(test_evio_rx_concat_map_outer_error_and_cancel_destroy_owned_backlog)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        queued_fixture fixture = {0};
        queued_init(&fixture, loop, 2);
        evio_rx_subscription_start(fixture.log.subscription);
        publish_owned(&fixture.outer, 1);
        publish_owned(&fixture.outer, 2);
        publish_owned(&fixture.outer, 3);
        assert_int_equal(fixture.values.stats.live, 2);
        if (i) {
            evio_rx_subscription_cancel(fixture.log.subscription);
        } else {
            fail_producer(&fixture.outer, EIO);
        }
        assert_int_equal(fixture.log.count, 0);
        assert_int_equal(fixture.log.completed, 0);
        assert_int_equal(fixture.log.error, i ? 0 : EIO);
        assert_int_equal(fixture.values.stats.copied, 2);
        assert_queued_stopped(&fixture);
        evio_rx_subscription_free(fixture.log.subscription);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_concat_map_copy_failure_and_cancel_during_copy_clear_partial_slot)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        queued_fixture fixture = {0};
        queued_init(&fixture, loop, 2);
        evio_rx_subscription_start(fixture.log.subscription);
        publish_owned(&fixture.outer, 1);
        fixture.values.fail_at = i ? 0 : 2;
        fixture.values.cancel_copy = i;
        publish_owned(&fixture.outer, 2);
        if (!i) {
            publish_owned(&fixture.outer, 3);
        }
        assert_int_equal(fixture.log.count, 0);
        assert_int_equal(fixture.log.completed, 0);
        assert_int_equal(fixture.log.error, i ? 0 : EIO);
        assert_int_equal(fixture.values.calls, i ? 1 : 2);
        assert_int_equal(fixture.values.stats.copied, 1);
        assert_queued_stopped(&fixture);
        evio_rx_subscription_free(fixture.log.subscription);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_concat_map_cancel_and_free_first_inner_next_destroy_owned_backlog)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        queued_fixture fixture = {0};
        queued_init(&fixture, loop, 2);
        evio_rx_subscription_start(fixture.log.subscription);
        publish_owned(&fixture.outer, 1);
        publish_owned(&fixture.outer, 2);
        publish_owned(&fixture.outer, 3);
        finish(&fixture.outer);
        fixture.log.action = i ? free_flat : cancel_flat;
        publish(&fixture.first, 1);
        assert_int_equal(fixture.log.count, 1);
        assert_int_equal(fixture.log.values[0], 1);
        assert_int_equal(fixture.log.completed, 0);
        assert_int_equal(fixture.log.error, 0);
        assert_int_equal(fixture.values.stats.copied, 2);
        assert_false(fixture.first.last_active);
        assert_queued_stopped(&fixture);
        if (i) {
            assert_null(fixture.log.subscription);
        } else {
            assert_true(evio_rx_subscription_is_closed(fixture.log.subscription));
            evio_rx_subscription_free(fixture.log.subscription);
        }
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_concat_map_invalid_capacity_retains_capture)
{
    evio_rx_observable *source = evio_rx_from_array(&int_type, NULL, 0);
    size_t destroyed = 0;
    evio_rx_project project = {
        .type = &int_type,
        .cb = project_burst,
        .capture = { .data = &destroyed, .destroy = count_capture },
    };
    assert_null(evio_rx_concat_map(source, project, 0));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_concat_map(source, project, SIZE_MAX));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_concat_map(NULL, project, 1));
    assert_int_equal(errno, EINVAL);
    project.cb = NULL;
    assert_null(evio_rx_concat_map(source, project, 1));
    assert_int_equal(errno, EINVAL);
    project.cb = project_burst;
    project.type = NULL;
    assert_null(evio_rx_concat_map(source, project, 1));
    assert_int_equal(errno, EINVAL);
    assert_int_equal(destroyed, 0);
    evio_rx_observable_free(source);
}

TEST(test_evio_rx_concat_map_failed_static_prepare_releases_queue_without_source_destroy)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    failure_project failure = { .mode = FAILURE_PREPARE };
    failure_capture *capture = evio_malloc(sizeof(*capture));
    capture->project = &failure;
    evio_rx_observable *source = evio_rx_observable_new(&int_type, failure_source,
                                                        (evio_rx_capture) { .data = capture, .destroy = failure_capture_free });
    size_t calls = 0;
    evio_rx_observable *concatenated = evio_rx_concat_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_burst,
        .capture = { .data = &calls },
    }, 2);
    flat_log log = {0};
    assert_null(evio_rx_subscription_new(loop, concatenated, flat_observer(&log), 1));
    assert_int_equal(errno, EACCES);
    assert_int_equal(log.count, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(log.error, 0);
    assert_int_equal(calls, 0);
    assert_int_equal(failure.prepared, 1);
    assert_int_equal(failure.started, 0);
    assert_int_equal(failure.stopped, 0);
    assert_int_equal(failure.destroyed, 0);
    assert_int_equal(failure.released, 0);
    evio_rx_observable_free(source);
    evio_rx_observable_free(concatenated);
    assert_int_equal(failure.released, 1);
    assert_int_equal(loop->refcount, 0);
    evio_loop_free(loop);
}

typedef struct {
    flat_driver inner[4];
    size_t concurrency;
    size_t calls;
    size_t peak;
    int order[4];
    int fail_at;
    int sync_from;
} pool_project;

static size_t pool_active(const pool_project *project)
{
    size_t active = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (project->inner[i].current) {
            active++;
        }
    }
    return active;
}

static evio_rx_observable *project_pool(void *ctx, const void *value)
{
    pool_project *project = ctx;
    int number = *(const int *)value;
    assert_true(number >= 1 && number <= 4);
    assert_true(project->calls < 4);
    project->order[project->calls] = number;
    project->calls++;
    if (number == project->fail_at) {
        errno = EIO;
        return NULL;
    }
    size_t active = pool_active(project);
    assert_true(active < project->concurrency);
    if (active + 1 > project->peak) {
        project->peak = active + 1;
    }
    if (project->sync_from && number >= project->sync_from) {
        return evio_rx_from_array(&int_type, &number, 1);
    }
    return evio_rx_observable_new(&int_type, flat_source,
                                  (evio_rx_capture) { .data = &project->inner[number - 1] });
}

static void assert_pool_stopped(const pool_project *project, size_t expected)
{
    size_t prepared = 0;
    for (size_t i = 0; i < 4; ++i) {
        const flat_driver *driver = &project->inner[i];
        assert_null(driver->current);
        assert_int_equal(driver->prepared, driver->started);
        assert_int_equal(driver->started, driver->stopped);
        assert_int_equal(driver->stopped, driver->destroyed);
        prepared += driver->prepared;
    }
    assert_int_equal(prepared, expected);
}

TEST(test_evio_rx_merge_map_limits_active_and_starts_waiting_fifo_with_out_of_order_results)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3, 4};
    pool_project project = { .concurrency = 2 };
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 4);
    evio_rx_observable *merged = evio_rx_merge_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_pool,
        .capture = { .data = &project },
    }, 2, 2);
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, merged, flat_observer(&log), 1);
    evio_rx_observable_free(source);
    evio_rx_observable_free(merged);
    evio_rx_subscription_start(log.subscription);
    assert_int_equal(project.calls, 2);
    assert_int_equal(pool_active(&project), 2);
    assert_int_equal(log.completed, 0);
    publish(&project.inner[1], 2);
    finish(&project.inner[1]);
    assert_int_equal(project.calls, 3);
    assert_non_null(project.inner[0].current);
    assert_non_null(project.inner[2].current);
    assert_int_equal(pool_active(&project), 2);
    publish(&project.inner[2], 3);
    finish(&project.inner[2]);
    assert_int_equal(project.calls, 4);
    assert_non_null(project.inner[3].current);
    assert_int_equal(pool_active(&project), 2);
    publish(&project.inner[3], 4);
    finish(&project.inner[3]);
    assert_int_equal(pool_active(&project), 1);
    assert_int_equal(log.completed, 0);
    publish(&project.inner[0], 1);
    finish(&project.inner[0]);
    int expected[] = {2, 3, 4, 1};
    assert_int_equal(log.count, 4);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_memory_equal(project.order, values, sizeof(values));
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(project.peak, 2);
    assert_pool_stopped(&project, 4);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_loop_free(loop);
}

TEST(test_evio_rx_merge_map_concurrency_one_preserves_concat_order)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3, 4};
    pool_project project = { .concurrency = 1 };
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 4);
    evio_rx_observable *merged = evio_rx_merge_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_pool,
        .capture = { .data = &project },
    }, 1, 3);
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, merged, flat_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    for (size_t i = 0; i < 4; ++i) {
        assert_int_equal(project.calls, i + 1);
        assert_int_equal(pool_active(&project), 1);
        publish(&project.inner[i], i + 1);
        finish(&project.inner[i]);
    }
    assert_int_equal(log.count, 4);
    assert_memory_equal(log.values, values, sizeof(values));
    assert_memory_equal(project.order, values, sizeof(values));
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(project.peak, 1);
    assert_pool_stopped(&project, 4);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(merged);
    evio_loop_free(loop);
}

TEST(test_evio_rx_merge_map_sync_and_empty_inner_release_slots_before_next_input)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {0, 1, 2, 3};
    size_t calls = 0;
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 4);
    evio_rx_observable *merged = evio_rx_merge_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_burst,
        .capture = { .data = &calls },
    }, 3, 1);
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, merged, flat_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    int expected[] = {1, 2, 3, 4, 5, 6};
    assert_int_equal(log.count, 6);
    assert_memory_equal(log.values, expected, sizeof(expected));
    assert_int_equal(log.completed, 1);
    assert_int_equal(log.error, 0);
    assert_int_equal(calls, 4);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(merged);
    evio_loop_free(loop);
}

typedef struct {
    flat_log log;
    pool_project *project;
} pool_log;

static void complete_two_inners(flat_log *base)
{
    pool_log *log = (pool_log *)base;
    if (base->values[base->count - 1] != 1) {
        return;
    }
    finish(&log->project->inner[0]);
    finish(&log->project->inner[1]);
    assert_int_equal(base->count, 1);
}

TEST(test_evio_rx_merge_map_queued_completions_and_sync_replacements_keep_both_slots)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    int values[] = {1, 2, 3, 4};
    pool_project project = { .concurrency = 2, .sync_from = 3 };
    evio_rx_observable *source = evio_rx_from_array(&int_type, values, 4);
    evio_rx_observable *merged = evio_rx_merge_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_pool,
        .capture = { .data = &project },
    }, 2, 2);
    pool_log log = { .log = { .action = complete_two_inners }, .project = &project };
    log.log.subscription = evio_rx_subscription_new(loop, merged, flat_observer(&log.log), 2);
    evio_rx_subscription_start(log.log.subscription);
    assert_int_equal(project.calls, 2);
    publish(&project.inner[0], 1);
    int expected[] = {1, 3, 4};
    assert_int_equal(log.log.count, 3);
    assert_memory_equal(log.log.values, expected, sizeof(expected));
    assert_memory_equal(project.order, values, sizeof(values));
    assert_int_equal(project.calls, 4);
    assert_int_equal(project.peak, 2);
    assert_int_equal(log.log.completed, 1);
    assert_int_equal(log.log.error, 0);
    assert_pool_stopped(&project, 2);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(log.log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(merged);
    evio_loop_free(loop);
}

static evio_rx_observable *project_owned_pool(void *ctx, const void *value)
{
    const large_value *number = value;
    assert_ptr_equal(number->self, number);
    assert_int_equal((uintptr_t)number % alignof(large_value), 0);
    assert_int_equal(number->bytes[0], 17);
    return project_pool(ctx, number->number);
}

typedef struct {
    flat_driver outer;
    pool_project project;
    queued_values values;
    evio_rx_type type;
    flat_log log;
} pool_fixture;

static void pool_init(pool_fixture *fixture, evio_loop *loop, size_t capacity)
{
    fixture->project.concurrency = 2;
    fixture->type = (evio_rx_type) {
        .size = sizeof(large_value), .alignment = alignof(large_value),
        .copy = queued_copy, .destroy = queued_destroy, .ctx = &fixture->values,
    };
    evio_rx_observable *source = evio_rx_observable_new(&fixture->type, flat_source,
                                                        (evio_rx_capture) { .data = &fixture->outer });
    evio_rx_observable *merged = evio_rx_merge_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_owned_pool,
        .capture = { .data = &fixture->project },
    }, 2, capacity);
    fixture->log.loop = loop;
    fixture->log.subscription = evio_rx_subscription_new(loop, merged, flat_observer(&fixture->log), 1);
    assert_non_null(fixture->log.subscription);
    fixture->values.subscription = fixture->log.subscription;
    evio_rx_observable_free(source);
    evio_rx_observable_free(merged);
}

static void assert_pool_failure_cleanup(const pool_fixture *fixture)
{
    assert_null(fixture->outer.current);
    assert_int_equal(fixture->outer.stopped, 1);
    assert_int_equal(fixture->outer.destroyed, 1);
    assert_pool_stopped(&fixture->project, 2);
    assert_int_equal(fixture->values.stats.live, 0);
    assert_int_equal(fixture->values.stats.destroyed, fixture->values.stats.copied);
    assert_int_equal(fixture->log.loop->refcount, 0);
}

TEST(test_evio_rx_merge_map_projects_owned_waiting_values_in_fifo_order)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    pool_fixture fixture = {0};
    pool_init(&fixture, loop, 2);
    evio_rx_subscription_start(fixture.log.subscription);
    publish_owned(&fixture.outer, 1);
    publish_owned(&fixture.outer, 2);
    publish_owned(&fixture.outer, 3);
    publish_owned(&fixture.outer, 4);
    assert_int_equal(fixture.project.calls, 2);
    assert_int_equal(fixture.values.stats.copied, 2);
    assert_int_equal(fixture.values.stats.live, 2);
    finish(&fixture.outer);
    int expected[] = {1, 3, 2, 4};
    for (size_t i = 0; i < 4; ++i) {
        flat_driver *driver = &fixture.project.inner[expected[i] - 1];
        publish(driver, expected[i]);
        finish(driver);
    }
    int projected[] = {1, 2, 3, 4};
    assert_int_equal(fixture.log.count, 4);
    assert_memory_equal(fixture.log.values, expected, sizeof(expected));
    assert_memory_equal(fixture.project.order, projected, sizeof(projected));
    assert_int_equal(fixture.project.calls, 4);
    assert_int_equal(fixture.project.peak, 2);
    assert_int_equal(fixture.log.completed, 1);
    assert_int_equal(fixture.log.error, 0);
    assert_int_equal(fixture.values.stats.copied, 2);
    assert_int_equal(fixture.values.stats.destroyed, 2);
    assert_int_equal(fixture.values.stats.live, 0);
    assert_pool_stopped(&fixture.project, 4);
    assert_int_equal(fixture.outer.stopped, 1);
    assert_int_equal(fixture.outer.destroyed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_rx_subscription_free(fixture.log.subscription);
    evio_loop_free(loop);
}

TEST(test_evio_rx_merge_map_capacity_overflow_cancels_all_active_and_waiting)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    pool_fixture fixture = {0};
    pool_init(&fixture, loop, 1);
    evio_rx_subscription_start(fixture.log.subscription);
    publish_owned(&fixture.outer, 1);
    publish_owned(&fixture.outer, 2);
    publish_owned(&fixture.outer, 3);
    assert_int_equal(fixture.values.stats.live, 1);
    publish_owned(&fixture.outer, 4);
    assert_int_equal(fixture.log.count, 0);
    assert_int_equal(fixture.log.completed, 0);
    assert_int_equal(fixture.log.error, EOVERFLOW);
    assert_int_equal(fixture.project.calls, 2);
    assert_int_equal(fixture.values.calls, 1);
    assert_int_equal(fixture.values.stats.copied, 1);
    assert_pool_failure_cleanup(&fixture);
    evio_rx_subscription_free(fixture.log.subscription);
    evio_loop_free(loop);
}

TEST(test_evio_rx_merge_map_outer_inner_and_waiting_projector_errors_destroy_all_resources)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 3; ++i) {
        pool_fixture fixture = {0};
        pool_init(&fixture, loop, 2);
        fixture.project.fail_at = i == 2 ? 3 : 0;
        evio_rx_subscription_start(fixture.log.subscription);
        publish_owned(&fixture.outer, 1);
        publish_owned(&fixture.outer, 2);
        publish_owned(&fixture.outer, 3);
        publish_owned(&fixture.outer, 4);
        assert_int_equal(fixture.project.calls, 2);
        assert_int_equal(fixture.values.stats.live, 2);
        switch (i) {
            case 0:
                fail_producer(&fixture.outer, EIO);
                break;
            case 1:
                fail_producer(&fixture.project.inner[0], EIO);
                break;
            default:
                finish(&fixture.project.inner[1]);
                break;
        }
        assert_int_equal(fixture.log.count, 0);
        assert_int_equal(fixture.log.completed, 0);
        assert_int_equal(fixture.log.error, EIO);
        assert_int_equal(fixture.project.calls, i == 2 ? 3 : 2);
        assert_int_equal(fixture.values.stats.copied, 2);
        assert_pool_failure_cleanup(&fixture);
        evio_rx_subscription_free(fixture.log.subscription);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_merge_map_cancel_and_free_with_active_and_waiting_values)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        pool_fixture fixture = {0};
        pool_init(&fixture, loop, 2);
        evio_rx_subscription_start(fixture.log.subscription);
        publish_owned(&fixture.outer, 1);
        publish_owned(&fixture.outer, 2);
        publish_owned(&fixture.outer, 3);
        publish_owned(&fixture.outer, 4);
        if (i) {
            free_flat(&fixture.log);
        } else {
            evio_rx_subscription_cancel(fixture.log.subscription);
        }
        assert_int_equal(fixture.log.count, 0);
        assert_int_equal(fixture.log.completed, 0);
        assert_int_equal(fixture.log.error, 0);
        assert_int_equal(fixture.project.calls, 2);
        assert_int_equal(fixture.values.stats.copied, 2);
        assert_pool_failure_cleanup(&fixture);
        if (i) {
            assert_null(fixture.log.subscription);
        } else {
            evio_rx_subscription_free(fixture.log.subscription);
        }
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_merge_map_cancel_and_free_first_inner_next_stop_other_active_and_waiting)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        pool_fixture fixture = {0};
        pool_init(&fixture, loop, 2);
        evio_rx_subscription_start(fixture.log.subscription);
        publish_owned(&fixture.outer, 1);
        publish_owned(&fixture.outer, 2);
        publish_owned(&fixture.outer, 3);
        publish_owned(&fixture.outer, 4);
        fixture.log.action = i ? free_flat : cancel_flat;
        publish(&fixture.project.inner[0], 1);
        assert_int_equal(fixture.log.count, 1);
        assert_int_equal(fixture.log.values[0], 1);
        assert_int_equal(fixture.log.completed, 0);
        assert_int_equal(fixture.log.error, 0);
        assert_int_equal(fixture.project.calls, 2);
        assert_int_equal(fixture.values.stats.copied, 2);
        assert_false(fixture.project.inner[0].last_active);
        assert_pool_failure_cleanup(&fixture);
        if (i) {
            assert_null(fixture.log.subscription);
        } else {
            evio_rx_subscription_free(fixture.log.subscription);
        }
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_merge_map_copy_failure_and_cancel_during_copy_release_two_active_inners)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        pool_fixture fixture = {0};
        pool_init(&fixture, loop, 2);
        fixture.values.fail_at = i ? 0 : 2;
        fixture.values.cancel_copy = i;
        evio_rx_subscription_start(fixture.log.subscription);
        publish_owned(&fixture.outer, 1);
        publish_owned(&fixture.outer, 2);
        publish_owned(&fixture.outer, 3);
        if (!i) {
            publish_owned(&fixture.outer, 4);
        }
        assert_int_equal(fixture.log.count, 0);
        assert_int_equal(fixture.log.completed, 0);
        assert_int_equal(fixture.log.error, i ? 0 : EIO);
        assert_int_equal(fixture.project.calls, 2);
        assert_int_equal(fixture.values.calls, i ? 1 : 2);
        assert_int_equal(fixture.values.stats.copied, 1);
        assert_pool_failure_cleanup(&fixture);
        evio_rx_subscription_free(fixture.log.subscription);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_merge_map_invalid_limits_do_not_take_capture)
{
    evio_rx_observable *source = evio_rx_from_array(&int_type, NULL, 0);
    size_t destroyed = 0;
    evio_rx_project project = {
        .type = &int_type,
        .cb = project_burst,
        .capture = { .data = &destroyed, .destroy = count_capture },
    };
    assert_null(evio_rx_merge_map(source, project, 0, 1));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_merge_map(source, project, 1, 0));
    assert_int_equal(errno, EINVAL);
    assert_null(evio_rx_merge_map(source, project, SIZE_MAX, 1));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_merge_map(source, project, 1, SIZE_MAX));
    assert_int_equal(errno, EOVERFLOW);
    assert_null(evio_rx_merge_map(NULL, project, 1, 1));
    assert_int_equal(errno, EINVAL);
    assert_int_equal(destroyed, 0);
    evio_rx_observable_free(source);
}

typedef enum {
    CLEANUP_NEXT,
    CLEANUP_INVALID,
    CLEANUP_OVERFLOW,
} cleanup_action;

typedef struct {
    flat_driver *outer;
    size_t destroyed;
    cleanup_action action;
} cleanup_log;

typedef struct {
    cleanup_log *log;
} cleanup_capture;

static void cleanup_publish(void *data)
{
    cleanup_capture *capture = data;
    cleanup_log *log = capture->log;
    evio_free(capture);
    log->destroyed++;
    switch (log->action) {
        case CLEANUP_NEXT:
            publish(log->outer, 2);
            break;
        case CLEANUP_INVALID: {
            assert_non_null(log->outer->current);
            evio_rx_emitter *emitter = log->outer->current->emitter;
            evio_rx_emitter_invoke(emitter, send_next, NULL);
            break;
        }
        case CLEANUP_OVERFLOW:
            publish(log->outer, 2);
            publish(log->outer, 3);
            break;
    }
}

static evio_rx_observable *project_cleanup(void *ctx, const void *value)
{
    if (*(const int *)value == 1) {
        cleanup_capture *capture = evio_malloc(sizeof(*capture));
        capture->log = ctx;
        return evio_rx_observable_new(&int_type, empty_source,
                                      (evio_rx_capture) { .data = capture, .destroy = cleanup_publish });
    }
    return evio_rx_from_array(&int_type, value, 1);
}

TEST(test_evio_rx_switch_map_destroy_reentry_delivers_reserved_error_and_drops_backlog)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    for (size_t i = 0; i < 2; ++i) {
        flat_driver outer = {0};
        cleanup_log cleanup = { .outer = &outer, .action = i ? CLEANUP_OVERFLOW : CLEANUP_INVALID };
        evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                            (evio_rx_capture) { .data = &outer });
        evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
            .type = &int_type,
            .cb = project_cleanup,
            .capture = { .data = &cleanup },
        });
        flat_log log = {0};
        log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
        evio_rx_subscription_start(log.subscription);
        publish(&outer, 1);
        int error = log.error;
        evio_rx_subscription_free(log.subscription);
        evio_rx_observable_free(source);
        evio_rx_observable_free(switched);
        assert_int_equal(error, i ? EOVERFLOW : EINVAL);
        assert_int_equal(log.count, 0);
        assert_int_equal(log.completed, 0);
        assert_int_equal(cleanup.destroyed, 1);
        assert_false(outer.last_active);
        assert_int_equal(outer.stopped, 1);
        assert_int_equal(outer.destroyed, 1);
        assert_int_equal(loop->refcount, 0);
    }
    evio_loop_free(loop);
}

TEST(test_evio_rx_switch_map_destroy_reentry_drains_before_producer_returns)
{
    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    flat_driver outer = {0};
    cleanup_log cleanup = { .outer = &outer };
    evio_rx_observable *source = evio_rx_observable_new(&int_type, flat_source,
                                                        (evio_rx_capture) { .data = &outer });
    evio_rx_observable *switched = evio_rx_switch_map(source, (evio_rx_project) {
        .type = &int_type,
        .cb = project_cleanup,
        .capture = { .data = &cleanup },
    });
    flat_log log = {0};
    log.subscription = evio_rx_subscription_new(loop, switched, flat_observer(&log), 1);
    evio_rx_subscription_start(log.subscription);
    publish(&outer, 1);
    size_t delivered = log.count;
    evio_rx_subscription_free(log.subscription);
    evio_rx_observable_free(source);
    evio_rx_observable_free(switched);
    assert_int_equal(delivered, 1);
    assert_int_equal(log.values[0], 2);
    assert_int_equal(log.error, 0);
    assert_int_equal(log.completed, 0);
    assert_int_equal(cleanup.destroyed, 1);
    assert_true(outer.last_active);
    assert_int_equal(outer.prepared, 1);
    assert_int_equal(outer.started, 1);
    assert_int_equal(outer.stopped, 1);
    assert_int_equal(outer.destroyed, 1);
    assert_int_equal(loop->refcount, 0);
    evio_loop_free(loop);
}
