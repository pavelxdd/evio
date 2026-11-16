#include "test.h"
#include "evio_rx.h"

#define TIMED_SEEDS 1000
#define TIMED_STEPS 80
#define TIMED_SUBS 5
#define TIMED_ACTIONS 200
#define TIMED_QUEUE 512
#define TIMED_START 100
#define TIMED_DRAIN 32

enum {
    OP_DEBOUNCE,
    OP_THROTTLE,
    OP_DELAY,
    OP_TIMEOUT,
    OP_COUNT,
};

// Entries pack `time << 32 | value`.
typedef struct {
    uint64_t *items;
    size_t count;
    size_t capacity;
} event_list;

typedef struct timed_state timed_state;

typedef struct {
    timed_state *state;
    evio_rx_subscription *subscription;
    int op;
    evio_time duration;
    size_t capacity;
    event_list tap;
    event_list out;
    evio_time start;
    evio_time source_end;
    int source_error;
    bool source_ended;
    bool used;
    bool dead;
    bool open;
    bool cut;                   /**< Cancelled while deliveries or ticks of the same time were pending. */
} timed_sub;

struct timed_state {
    evio_loop *loop;
    evio_rx_subject *subject;
    evio_rx_observable *source;
    timed_sub subs[TIMED_SUBS];
    uint64_t rng;
    uint64_t next_value;
    size_t actions;
    size_t depth;
    int error;
    bool closed;
    bool ticking;               /**< Timers of the current time are firing. */
};

static uint64_t random_next(timed_state *state, uint64_t n)
{
    state->rng ^= state->rng << 13;
    state->rng ^= state->rng >> 7;
    state->rng ^= state->rng << 17;
    return state->rng % n;
}

static void list_push(event_list *list, uint64_t entry)
{
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 16;
        list->items = evio_reallocarray(list->items, list->capacity, sizeof(*list->items));
    }
    list->items[list->count++] = entry;
}

static void list_free(event_list *list)
{
    evio_free(list->items);
    *list = (event_list) {0};
}

static evio_time entry_time(uint64_t entry)
{
    return entry >> 32;
}

static uint64_t entry_value(uint64_t entry)
{
    return entry & UINT32_MAX;
}

// Reference timeline of a run up to `horizon`, assuming a tick at every time unit.
typedef struct {
    const timed_sub *sub;
    evio_time horizon;
    event_list *out;
    bool ended;
    evio_time end_time;
    int error;
} timed_model;

static void model_emit(timed_model *model, evio_time time, uint64_t value)
{
    if (time <= model->horizon) {
        list_push(model->out, (time << 32) | value);
    }
}

static void model_end(timed_model *model, evio_time time, int error)
{
    if (time <= model->horizon) {
        model->ended = true;
        model->end_time = time;
        model->error = error;
    }
}

static void model_debounce(timed_model *model)
{
    const timed_sub *sub = model->sub;
    bool pending = false;
    evio_time deadline = 0;
    uint64_t value = 0;
    for (size_t i = 0; i < sub->tap.count; ++i) {
        evio_time time = entry_time(sub->tap.items[i]);
        if (pending && deadline <= time) {
            model_emit(model, deadline, value);
        }
        pending = true;
        deadline = time + sub->duration;
        value = entry_value(sub->tap.items[i]);
    }
    if (!sub->source_ended) {
        if (pending) {
            model_emit(model, deadline, value);
        }
        return;
    }
    if (pending && deadline <= sub->source_end) {
        model_emit(model, deadline, value);
    } else if (pending && !sub->source_error) {
        model_emit(model, sub->source_end, value);
    }
    model_end(model, sub->source_end, sub->source_error);
}

static void model_throttle(timed_model *model)
{
    const timed_sub *sub = model->sub;
    evio_time deadline = 0;
    for (size_t i = 0; i < sub->tap.count; ++i) {
        evio_time time = entry_time(sub->tap.items[i]);
        if (time >= deadline) {
            model_emit(model, time, entry_value(sub->tap.items[i]));
            deadline = time + sub->duration;
        }
    }
    if (sub->source_ended) {
        model_end(model, sub->source_end, sub->source_error);
    }
}

// A tick emits one due value, so values due together leave one time unit apart.
typedef struct {
    evio_time times[4];
    uint64_t values[4];
    size_t head;
    size_t count;
} delay_queue;

static void delay_flush(timed_model *model, delay_queue *queue, evio_time until)
{
    while (queue->count && queue->times[queue->head] <= until) {
        model_emit(model, queue->times[queue->head], queue->values[queue->head]);
        queue->head = (queue->head + 1) % 4;
        queue->count--;
    }
}

static void model_delay(timed_model *model)
{
    const timed_sub *sub = model->sub;
    delay_queue queue = {0};
    evio_time last = 0;
    for (size_t i = 0; i < sub->tap.count; ++i) {
        evio_time time = entry_time(sub->tap.items[i]);
        delay_flush(model, &queue, time);
        if (queue.count == sub->capacity) {
            model_end(model, time, EOVERFLOW);
            return;
        }
        evio_time due = time + sub->duration;
        last = due > last ? due : last + 1;
        size_t slot = (queue.head + queue.count++) % 4;
        queue.times[slot] = last;
        queue.values[slot] = entry_value(sub->tap.items[i]);
    }
    if (!sub->source_ended) {
        delay_flush(model, &queue, EVIO_TIME_MAX);
        return;
    }
    delay_flush(model, &queue, sub->source_end);
    if (sub->source_error) {
        model_end(model, sub->source_end, sub->source_error);
        return;
    }
    evio_time finish = queue.count ? last : sub->source_end;
    delay_flush(model, &queue, EVIO_TIME_MAX);
    model_end(model, finish, 0);
}

static void model_timeout(timed_model *model)
{
    const timed_sub *sub = model->sub;
    evio_time deadline = sub->start + sub->duration;
    for (size_t i = 0; i < sub->tap.count; ++i) {
        evio_time time = entry_time(sub->tap.items[i]);
        if (time >= deadline) {
            break;
        }
        model_emit(model, time, entry_value(sub->tap.items[i]));
        deadline = time + sub->duration;
    }
    if (sub->source_ended && sub->source_end < deadline) {
        model_end(model, sub->source_end, sub->source_error);
    } else {
        model_end(model, deadline, ETIMEDOUT);
    }
}

// Checks the run so far; `error` is the observed terminal, or -1 for none.
static void check_run(timed_sub *sub, int error)
{
    evio_time now = evio_get_time(sub->state->loop);
    event_list expected = {0};
    timed_model model = { .sub = sub, .horizon = now, .out = &expected };
    switch (sub->op) {
        case OP_DEBOUNCE:
            model_debounce(&model);
            break;
        case OP_THROTTLE:
            model_throttle(&model);
            break;
        case OP_DELAY:
            model_delay(&model);
            break;
        case OP_TIMEOUT:
            model_timeout(&model);
            break;
        default:
            fail();
    }
    if (sub->cut) {
        size_t before = 0;
        while (before < expected.count && entry_time(expected.items[before]) < now) {
            before++;
        }
        assert_true(sub->out.count >= before);
        assert_true(sub->out.count <= expected.count);
        assert_true(!model.ended || model.end_time == now);
    } else {
        assert_int_equal(sub->out.count, expected.count);
        assert_int_equal(model.ended, error >= 0);
        if (model.ended) {
            assert_int_equal(model.end_time, now);
            assert_int_equal(model.error, error);
        }
    }
    for (size_t i = 0; i < sub->out.count; ++i) {
        assert_int_equal(sub->out.items[i], expected.items[i]);
    }
    list_free(&expected);
}

static void run_begin(timed_sub *sub)
{
    timed_state *state = sub->state;
    sub->tap.count = 0;
    sub->out.count = 0;
    sub->start = evio_get_time(state->loop);
    sub->source_ended = state->closed;
    sub->source_end = sub->start;
    sub->source_error = state->error;
    sub->open = true;
    sub->cut = false;
}

static void run_cancel(timed_sub *sub)
{
    if (!sub->open) {
        return;
    }
    sub->cut = sub->state->depth > 0 || sub->state->ticking;
    check_run(sub, -1);
    sub->open = false;
}

static void enter(timed_sub *sub)
{
    assert_true(sub->used);
    assert_false(sub->dead);
    assert_true(sub->open);
    sub->state->depth++;
}

static void leave(timed_sub *sub)
{
    sub->state->depth--;
}

static void publish(timed_state *state, size_t count)
{
    uint64_t values[4];
    for (size_t i = 0; i < count; ++i) {
        values[i] = state->next_value++;
    }
    bool closed = state->closed;
    int result;
    if (count == 1) {
        result = evio_rx_subject_next(state->subject, values);
    } else {
        result = evio_rx_subject_next_batch(state->subject, values, count);
    }
    if (closed) {
        assert_int_equal(result, -1);
        assert_int_equal(errno, EPIPE);
    } else {
        assert_int_equal(result, 0);
    }
}

static void terminate(timed_state *state, bool complete)
{
    if (!state->closed) {
        state->closed = true;
        state->error = complete ? 0 : EIO;
        evio_time now = evio_get_time(state->loop);
        for (size_t i = 0; i < TIMED_SUBS; ++i) {
            timed_sub *sub = &state->subs[i];
            if (sub->used && !sub->dead && sub->open && !sub->source_ended) {
                sub->source_ended = true;
                sub->source_end = now;
                sub->source_error = state->error;
            }
        }
    }
    if (complete) {
        evio_rx_subject_complete(state->subject);
    } else {
        evio_rx_subject_error(state->subject, EIO);
    }
}

static void tick(timed_state *state)
{
    state->ticking = true;
    state->loop->time++;
    evio_timer_update(state->loop);
    evio_invoke_pending(state->loop);
    state->ticking = false;
}

static void sub_cancel(timed_sub *sub)
{
    run_cancel(sub);
    evio_rx_subscription_cancel(sub->subscription);
}

static void sub_restart(timed_sub *sub)
{
    run_cancel(sub);
    run_begin(sub);
    evio_rx_subscription_restart(sub->subscription);
}

static void sub_free(timed_sub *sub)
{
    run_cancel(sub);
    sub->dead = true;
    evio_rx_subscription_free(sub->subscription);
    sub->subscription = NULL;
}

static void sub_action(timed_sub *sub, uint64_t kind)
{
    if (kind == 0) {
        sub_cancel(sub);
    } else if (kind == 1) {
        sub_restart(sub);
    } else {
        sub_free(sub);
    }
}

static timed_sub *pick(timed_state *state, const timed_sub *other)
{
    timed_sub *sub = &state->subs[random_next(state, TIMED_SUBS)];
    if (!sub->used || sub->dead || sub == other) {
        return NULL;
    }
    return sub;
}

static void subscribe(timed_state *state);

// Inside a tick, timers of other runs due at the same time may not have fired yet.
static void act(timed_sub *sub)
{
    timed_state *state = sub->state;
    if (state->actions == TIMED_ACTIONS || random_next(state, 100) >= 20) {
        return;
    }
    state->actions++;
    uint64_t choice = random_next(state, 7);
    if (choice == 0 && !state->ticking) {
        publish(state, 1 + random_next(state, 4));
    } else if (choice == 1) {
        terminate(state, state->ticking || random_next(state, 2));
    } else if (choice == 2) {
        subscribe(state);
    } else if (choice < 6) {
        sub_action(sub, choice - 3);
    } else {
        timed_sub *other = pick(state, sub);
        if (other) {
            sub_action(other, random_next(state, 3));
        }
    }
}

static int timed_tap(void *ctx, void *out, const void *values, size_t count)
{
    timed_sub *sub = ctx;
    timed_state *state = sub->state;
    enter(sub);
    assert_false(state->ticking);
    evio_time now = evio_get_time(state->loop);
    const uint64_t *value = values;
    for (size_t i = 0; i < count; ++i) {
        list_push(&sub->tap, (now << 32) | value[i]);
    }
    memcpy(out, values, count * sizeof(*value));
    act(sub);
    leave(sub);
    return 0;
}

static void timed_next(void *ctx, const void *value)
{
    timed_sub *sub = ctx;
    enter(sub);
    evio_time now = evio_get_time(sub->state->loop);
    list_push(&sub->out, (now << 32) | *(const uint64_t *)value);
    act(sub);
    leave(sub);
}

static void timed_end(timed_sub *sub, int error)
{
    enter(sub);
    assert_true(evio_rx_subscription_is_closed(sub->subscription));
    check_run(sub, error);
    sub->open = false;
    act(sub);
    leave(sub);
}

static void timed_error(void *ctx, int error)
{
    timed_end(ctx, error);
}

static void timed_complete(void *ctx)
{
    timed_end(ctx, 0);
}

static void subscribe(timed_state *state)
{
    timed_sub *sub = NULL;
    for (size_t i = 0; i < TIMED_SUBS && !sub; ++i) {
        sub = state->subs[i].used ? NULL : &state->subs[i];
    }
    if (!sub) {
        return;
    }
    *sub = (timed_sub) {
        .state = state,
        .used = true,
        .op = (int)random_next(state, OP_COUNT),
        .duration = 1 + random_next(state, 4),
        .capacity = 1 + random_next(state, 3),
        .tap = sub->tap,
        .out = sub->out,
    };
    evio_rx_observable *tap = evio_rx_map_batch(state->source, &evio_rx_uint64_type, timed_tap,
                                                (evio_rx_capture) { .data = sub });
    assert_non_null(tap);
    evio_rx_observable *source;
    switch (sub->op) {
        case OP_DEBOUNCE:
            source = evio_rx_debounce_time(tap, sub->duration);
            break;
        case OP_THROTTLE:
            source = evio_rx_throttle_time(tap, sub->duration);
            break;
        case OP_DELAY:
            source = evio_rx_delay(tap, sub->duration, sub->capacity);
            break;
        default:
            source = evio_rx_timeout(tap, sub->duration);
            break;
    }
    assert_non_null(source);
    evio_rx_observable_free(tap);
    evio_rx_observer observer = {
        .next = timed_next,
        .error = timed_error,
        .complete = timed_complete,
        .ctx = sub,
    };
    sub->subscription = evio_rx_subscription_new(state->loop, source, observer, TIMED_QUEUE);
    evio_rx_observable_free(source);
    assert_non_null(sub->subscription);
    run_begin(sub);
    evio_rx_subscription_start(sub->subscription);
}

static void check_quiescent(timed_state *state)
{
    for (size_t i = 0; i < TIMED_SUBS; ++i) {
        timed_sub *sub = &state->subs[i];
        if (sub->dead) {
            sub->used = false;
            sub->dead = false;
        }
        if (!sub->used) {
            continue;
        }
        assert_int_equal(evio_rx_subscription_is_closed(sub->subscription), !sub->open);
        if (sub->open) {
            check_run(sub, -1);
        }
    }
}

static void step(timed_state *state)
{
    uint64_t choice = random_next(state, 100);
    if (choice < 35) {
        publish(state, 1 + (random_next(state, 4) ? 0 : random_next(state, 4)));
    } else if (choice < 65) {
        for (uint64_t n = 1 + random_next(state, 3); n; --n) {
            tick(state);
            check_quiescent(state);
        }
    } else if (choice < 69) {
        terminate(state, true);
    } else if (choice < 72) {
        terminate(state, false);
    } else if (choice < 78) {
        assert_int_equal(evio_rx_subject_reopen(state->subject), 0);
        state->closed = false;
        state->error = 0;
        for (size_t i = 0; i < TIMED_SUBS; ++i) {
            timed_sub *closed = &state->subs[i];
            if (closed->used && !closed->dead && !closed->open && random_next(state, 2)) {
                sub_restart(closed);
            }
        }
    } else if (choice < 86) {
        subscribe(state);
    } else {
        timed_sub *sub = pick(state, NULL);
        if (sub) {
            sub_action(sub, random_next(state, 3));
        }
    }
}

static void timed_run(uint64_t seed)
{
    timed_state state = {
        .loop = evio_loop_new(EVIO_FLAG_NONE),
        .rng = seed * 0x9e3779b97f4a7c15ULL,
        .next_value = 1,
    };
    state.loop->time = TIMED_START;
    state.subject = evio_rx_subject_new(state.loop, &evio_rx_uint64_type, TIMED_QUEUE);
    state.source = evio_rx_from_subject(state.subject);
    assert_non_null(state.source);

    for (size_t i = 0; i < TIMED_STEPS; ++i) {
        step(&state);
        check_quiescent(&state);
    }
    terminate(&state, true);
    for (size_t i = 0; i < TIMED_DRAIN; ++i) {
        tick(&state);
        check_quiescent(&state);
    }
    for (size_t i = 0; i < TIMED_SUBS; ++i) {
        timed_sub *sub = &state.subs[i];
        if (sub->used && !sub->dead) {
            assert_false(sub->open);
            sub_free(sub);
        }
        list_free(&sub->tap);
        list_free(&sub->out);
    }
    evio_rx_observable_free(state.source);
    evio_rx_subject_free(state.subject);
    assert_int_equal(state.loop->refcount, 0);
    evio_loop_free(state.loop);
}

TEST(test_evio_rx_random_time_operators_match_model)
{
    for (uint64_t seed = 1; seed <= TIMED_SEEDS; ++seed) {
        timed_run(seed);
    }
}
