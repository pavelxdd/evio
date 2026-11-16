#include "test.h"
#include "evio_rx.h"

#define RANDOM_SEEDS 500
#define RANDOM_STEPS 60
#define RANDOM_SUBS 6
#define RANDOM_STAGES 4
#define RANDOM_ACTIONS 300
#define RANDOM_IDS 400
#define RANDOM_QUEUE 512
#define SIDE_FIRST 10000
#define PAIR_BASE 100000

enum {
    STAGE_MAP,
    STAGE_FILTER,
    STAGE_SCAN,
    STAGE_DISTINCT,
    STAGE_MAP_BATCH,
    STAGE_FILTER_BATCH,
    STAGE_SWITCH,
    STAGE_SWITCH_SAME,
    STAGE_CONCAT,
    STAGE_MERGE_MAP,
    STAGE_FAIL,
    STAGE_TAKE_UNTIL,
    STAGE_MERGE,
    STAGE_ZIP,
    STAGE_LATEST,
    STAGE_COUNT,
};

enum {
    ACTION_PUBLISH,
    ACTION_PUBLISH_BATCH,
    ACTION_PUBLISH_SIDE,
    ACTION_NOTIFY,
    ACTION_TERMINATE,
    ACTION_REOPEN,
    ACTION_SUBSCRIBE,
    ACTION_CANCEL,
    ACTION_RESTART,
    ACTION_FREE,
    ACTION_OTHER,
};

enum {
    END_NONE,
    END_COMPLETE,
    END_ERROR,
};

typedef struct {
    uint64_t *items;
    size_t count;
    size_t capacity;
} value_list;

typedef struct random_state random_state;
typedef struct random_sub random_sub;

typedef struct {
    random_state *state;
    random_sub *sub;
    size_t index;
    int kind;
    uint64_t arg;
} random_stage;

// A run of the user side source.
typedef struct {
    random_state *state;
    evio_rx_emitter *emitter;
    size_t slot;
    bool started;
} side_run;

typedef struct {
    side_run **items;
    size_t count;
    size_t capacity;
} side_list;

// `tap` logs input values in arrival order: `value` for the source and
// `(index + 1) << 32 | value` for the side input of stage `index`.
struct random_sub {
    random_state *state;
    evio_rx_subscription *subscription;
    random_stage stages[RANDOM_STAGES];
    size_t stage_count;
    bool take_until;
    bool early_end;             /**< zip or combine_latest may complete on the side input. */
    bool deferred;              /**< Restarted from its own callbacks; starts after an unknown id. */
    uint64_t last_id;
    value_list tap;
    value_list out;
    size_t depth;
    size_t notifications;
    uint64_t first_id;
    bool used;
    bool dead;
    bool open;
    bool cut;
    int end;
    int error;
};

struct random_state {
    evio_loop *loop;
    evio_rx_type type;
    evio_rx_subject *subject;
    evio_rx_subject *notifier;
    evio_rx_subject *side;
    evio_rx_observable *source;
    evio_rx_observable *stop;
    evio_rx_observable *side_subject;
    evio_rx_observable *side_user;
    side_list side_runs;
    size_t side_live;
    evio_rx_subscription *notifications_counter;
    random_sub subs[RANDOM_SUBS];
    uint64_t rng;
    uint64_t next_id;
    uint64_t complete_id;
    uint64_t side_next_id;
    int side_error;
    size_t boxes;
    size_t actions;
    size_t notifications;
    size_t depth;
    size_t subject_calls;       /**< Calls of `subject` on the stack; nonzero while it delivers. */
    bool closed;
    bool completed;
    bool side_closed;
    bool side_failed;
    bool failures;
};

static uint64_t random_next(random_state *state, uint64_t n)
{
    state->rng ^= state->rng << 13;
    state->rng ^= state->rng >> 7;
    state->rng ^= state->rng << 17;
    return state->rng % n;
}

static void list_push(value_list *list, uint64_t value)
{
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 16;
        list->items = evio_reallocarray(list->items, list->capacity, sizeof(*list->items));
    }
    list->items[list->count++] = value;
}

static void list_free(value_list *list)
{
    evio_free(list->items);
    *list = (value_list) {0};
}

static uint64_t box_value(const void *value)
{
    return **(uint64_t *const *)value;
}

static int box_new(random_state *state, void *out, uint64_t value)
{
    if (state->failures && !random_next(state, 40)) {
        return ENOMEM;
    }
    uint64_t *box = evio_malloc(sizeof(*box));
    *box = value;
    *(uint64_t **)out = box;
    state->boxes++;
    return 0;
}

static int box_copy(void *ctx, void *dst, const void *src)
{
    return box_new(ctx, dst, box_value(src));
}

static void box_destroy(void *ctx, void *value)
{
    random_state *state = ctx;
    assert_true(state->boxes > 0);
    evio_free(*(uint64_t **)value);
    state->boxes--;
}

static void boxes_destroy(random_state *state, void *values, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        box_destroy(state, (uint64_t **)values + i);
    }
}

// Reference semantics of the stages over the values seen by the tap.
typedef struct {
    const random_sub *sub;
    uint64_t scan[RANDOM_STAGES];
    uint64_t last[RANDOM_STAGES];
    bool has_last[RANDOM_STAGES];
    uint64_t latest[RANDOM_STAGES][2];
    bool has_latest[RANDOM_STAGES][2];
    value_list zip[RANDOM_STAGES][2];
    size_t zip_head[RANDOM_STAGES][2];
    value_list *out;
    int error;
} random_model;

static void model_push(random_model *model, size_t index, uint64_t value);

// Value `value` of input `input` (0 for the chain, 1 for the side) of a fan-in stage.
static void model_fan_in(random_model *model, size_t index, size_t input, uint64_t value)
{
    if (model->error) {
        return;
    }
    switch (model->sub->stages[index].kind) {
        case STAGE_MERGE:
            model_push(model, index + 1, value);
            break;
        case STAGE_LATEST: {
            uint64_t *latest = model->latest[index];
            bool *has = model->has_latest[index];
            latest[input] = value;
            has[input] = true;
            if (has[0] && has[1]) {
                model_push(model, index + 1, (latest[0] * PAIR_BASE) + latest[1]);
            }
            break;
        }
        case STAGE_ZIP: {
            value_list *other = &model->zip[index][!input];
            size_t *head = &model->zip_head[index][!input];
            if (*head == other->count) {
                list_push(&model->zip[index][input], value);
                break;
            }
            uint64_t pair[2];
            pair[input] = value;
            pair[!input] = other->items[(*head)++];
            model_push(model, index + 1, (pair[0] * PAIR_BASE) + pair[1]);
            break;
        }
        default:
            fail();
    }
}

static void model_push(random_model *model, size_t index, uint64_t value)
{
    if (model->error) {
        return;
    }
    if (index == model->sub->stage_count) {
        list_push(model->out, value);
        return;
    }
    const random_stage *stage = &model->sub->stages[index];
    switch (stage->kind) {
        case STAGE_MAP:
            model_push(model, index + 1, value + stage->arg);
            break;
        case STAGE_FILTER:
        case STAGE_FILTER_BATCH:
            if (value % stage->arg) {
                model_push(model, index + 1, value);
            }
            break;
        case STAGE_SCAN:
            model->scan[index] += value;
            model_push(model, index + 1, model->scan[index]);
            break;
        case STAGE_DISTINCT:
            if (model->has_last[index] && model->last[index] / 2 == value / 2) {
                break;
            }
            model->has_last[index] = true;
            model->last[index] = value;
            model_push(model, index + 1, value);
            break;
        case STAGE_MAP_BATCH:
            model_push(model, index + 1, value * 3);
            break;
        case STAGE_SWITCH:
        case STAGE_CONCAT:
        case STAGE_MERGE_MAP:
            model_push(model, index + 1, value);
            model_push(model, index + 1, value + 1);
            break;
        case STAGE_SWITCH_SAME:
            model_push(model, index + 1, 1000);
            model_push(model, index + 1, 1001);
            break;
        case STAGE_FAIL:
            if (value % stage->arg) {
                model_push(model, index + 1, value);
            } else {
                model->error = EDOM;
            }
            break;
        case STAGE_MERGE:
        case STAGE_ZIP:
        case STAGE_LATEST:
            model_fan_in(model, index, 0, value);
            break;
        default:
            model_push(model, index + 1, value);
            break;
    }
}

static void check_run(random_sub *sub)
{
    random_state *state = sub->state;
    value_list expected = {0};
    random_model model = { .sub = sub, .out = &expected };
    for (size_t i = 0; i < sub->tap.count; ++i) {
        uint64_t entry = sub->tap.items[i];
        size_t input = entry >> 32;
        if (input) {
            model_fan_in(&model, input - 1, 1, entry & UINT32_MAX);
        } else {
            model_push(&model, 0, entry);
        }
    }
    for (size_t i = 0; i < RANDOM_STAGES; ++i) {
        list_free(&model.zip[i][0]);
        list_free(&model.zip[i][1]);
    }
    bool notified = sub->take_until && state->notifications != sub->notifications;
    bool exact = !sub->cut && !state->failures && !notified;
    if (exact) {
        assert_int_equal(sub->out.count, expected.count);
    } else {
        assert_true(sub->out.count <= expected.count);
    }
    for (size_t i = 0; i < sub->out.count; ++i) {
        assert_int_equal(sub->out.items[i], expected.items[i]);
    }
    list_free(&expected);

    if (sub->end == END_ERROR) {
        bool allowed = sub->error == EIO || sub->error == EDOM;
        assert_true(allowed || (state->failures && sub->error == ENOMEM));
    }
    if (sub->end == END_COMPLETE) {
        bool side_completed = state->side_closed && !state->side_error;
        assert_true(state->completed || notified || (sub->early_end && side_completed));
    }
    if (!exact) {
        return;
    }
    if (model.error) {
        if (sub->early_end && sub->end == END_COMPLETE) {
            return;
        }
        assert_int_equal(sub->end, END_ERROR);
        assert_int_equal(sub->error, model.error);
        return;
    }
    if (sub->end == END_ERROR) {
        assert_int_equal(sub->error, EIO);
    }
    if (sub->end == END_COMPLETE && !sub->early_end) {
        uint64_t last = sub->first_id < state->complete_id ? state->complete_id : sub->first_id;
        uint64_t id = sub->first_id;
        if (sub->deferred) {
            id = last;
            for (size_t i = sub->tap.count; i--;) {
                if (sub->tap.items[i] <= UINT32_MAX) {
                    id = sub->tap.items[i];
                }
            }
            assert_true(id >= sub->first_id);
        }
        for (size_t i = 0; i < sub->tap.count; ++i) {
            if (sub->tap.items[i] <= UINT32_MAX) {
                assert_int_equal(sub->tap.items[i], id++);
            }
        }
        assert_int_equal(id, last);
    }
}

static void run_begin(random_sub *sub)
{
    random_state *state = sub->state;
    sub->tap.count = 0;
    sub->out.count = 0;
    sub->last_id = 0;
    sub->deferred = sub->depth > 0;
    sub->notifications = state->notifications;
    sub->first_id = state->next_id;
    sub->open = true;
    sub->cut = false;
    sub->end = END_NONE;
    sub->error = 0;
}

static void run_cancel(random_sub *sub)
{
    if (!sub->open) {
        return;
    }
    sub->cut = sub->depth > 0;
    check_run(sub);
    sub->open = false;
}

static void enter(random_sub *sub)
{
    assert_true(sub->used);
    assert_false(sub->dead);
    assert_true(sub->open);
    sub->depth++;
    sub->state->depth++;
}

static void leave(random_sub *sub)
{
    sub->depth--;
    sub->state->depth--;
}

static void publish(random_state *state, size_t count)
{
    if (state->next_id + count > RANDOM_IDS) {
        return;
    }
    uint64_t values[8];
    uint64_t *boxes[8];
    for (size_t i = 0; i < count; ++i) {
        values[i] = state->next_id + i;
        boxes[i] = &values[i];
    }
    state->next_id += count;
    bool closed = state->closed;
    int result;
    state->subject_calls++;
    if (count == 1) {
        result = evio_rx_subject_next(state->subject, boxes);
    } else {
        result = evio_rx_subject_next_batch(state->subject, boxes, count);
    }
    state->subject_calls--;
    if (closed) {
        assert_int_equal(result, -1);
        assert_int_equal(errno, EPIPE);
        return;
    }
    if (result) {
        assert_true(state->failures);
        assert_int_equal(errno, ENOMEM);
        state->closed = true;
        state->completed = false;
    }
}

typedef struct {
    uint64_t **boxes;
    size_t count;
} side_values;

static void side_emit(evio_rx_emitter *emitter, void *data, void *arg)
{
    const side_values *values = arg;
    for (size_t i = 0; i < values->count; ++i) {
        if (!evio_rx_emitter_next(emitter, &values->boxes[i])) {
            return;
        }
    }
}

static void side_end(evio_rx_emitter *emitter, void *data, void *arg)
{
    const random_state *state = arg;
    if (state->side_error) {
        evio_rx_emitter_error(emitter, state->side_error);
    } else {
        evio_rx_emitter_complete(emitter);
    }
}

// Runs started during the call are skipped, like Subject subscribers.
static void side_each(random_state *state, evio_rx_source_cb cb, void *arg)
{
    size_t count = state->side_runs.count;
    for (size_t i = 0; i < count; ++i) {
        side_run *run = state->side_runs.items[i];
        if (run) {
            evio_rx_emitter_invoke(run->emitter, cb, arg);
        }
    }
}

static void publish_side(random_state *state, size_t count)
{
    if (state->side_next_id + count > SIDE_FIRST + RANDOM_IDS) {
        return;
    }
    uint64_t values[4];
    uint64_t *boxes[4];
    for (size_t i = 0; i < count; ++i) {
        values[i] = state->side_next_id + i;
        boxes[i] = &values[i];
    }
    state->side_next_id += count;
    bool closed = state->side_closed || state->side_failed;
    int result;
    if (count == 1) {
        result = evio_rx_subject_next(state->side, boxes);
    } else {
        result = evio_rx_subject_next_batch(state->side, boxes, count);
    }
    if (closed) {
        assert_int_equal(result, -1);
        assert_int_equal(errno, EPIPE);
    } else if (result) {
        assert_true(state->failures);
        assert_int_equal(errno, ENOMEM);
        state->side_failed = true;
    }
    side_each(state, side_emit, &(side_values) { .boxes = boxes, .count = count });
}

static void terminate_side(random_state *state, bool complete)
{
    if (!state->side_closed) {
        state->side_closed = true;
        state->side_error = complete ? 0 : EIO;
    }
    if (complete) {
        evio_rx_subject_complete(state->side);
    } else {
        evio_rx_subject_error(state->side, EIO);
    }
    side_each(state, side_end, state);
}

static int side_prepare(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **data)
{
    random_state *state = ctx;
    if (state->failures && !random_next(state, 40)) {
        return ENOMEM;
    }
    side_run *run = evio_calloc(1, sizeof(*run));
    run->state = state;
    run->emitter = emitter;
    *data = run;
    state->side_live++;
    return 0;
}

static void side_start(evio_rx_emitter *emitter, void *data)
{
    side_run *run = data;
    random_state *state = run->state;
    assert_false(run->started);
    run->started = true;
    if (state->side_closed) {
        side_end(emitter, data, state);
        return;
    }
    side_list *list = &state->side_runs;
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 16;
        list->items = evio_reallocarray(list->items, list->capacity, sizeof(*list->items));
    }
    run->slot = list->count;
    list->items[list->count++] = run;
}

static void side_stop(void *data)
{
    side_run *run = data;
    side_list *list = &run->state->side_runs;
    assert_true(run->started);
    run->started = false;
    if (run->slot < list->count && list->items[run->slot] == run) {
        list->items[run->slot] = NULL;
    }
}

static void side_destroy(void *data)
{
    side_run *run = data;
    assert_false(run->started);
    run->state->side_live--;
    evio_free(run);
}

static void notify(random_state *state)
{
    uint64_t zero = 0;
    uint64_t *box = &zero;
    if (evio_rx_subject_next(state->notifier, &box)) {
        assert_true(state->failures);
        assert_true(errno == ENOMEM || errno == EPIPE);
    }
}

// Subscribed first, so it counts a notification before any run receives it.
static void count_notification(void *ctx, const void *value)
{
    random_state *state = ctx;
    state->notifications++;
}

static void terminate(random_state *state, bool complete)
{
    if (!state->closed) {
        state->closed = true;
        state->completed = complete;
        if (complete) {
            state->complete_id = state->next_id;
        }
    }
    state->subject_calls++;
    if (complete) {
        evio_rx_subject_complete(state->subject);
    } else {
        evio_rx_subject_error(state->subject, EIO);
    }
    state->subject_calls--;
    terminate_side(state, complete);
}

static void sub_cancel(random_sub *sub)
{
    run_cancel(sub);
    evio_rx_subscription_cancel(sub->subscription);
}

static void sub_restart(random_sub *sub)
{
    run_cancel(sub);
    run_begin(sub);
    evio_rx_subscription_restart(sub->subscription);
}

static void sub_free(random_sub *sub)
{
    run_cancel(sub);
    sub->dead = true;
    evio_rx_subscription_free(sub->subscription);
    sub->subscription = NULL;
}

static random_sub *pick(random_state *state, const random_sub *other)
{
    random_sub *sub = &state->subs[random_next(state, RANDOM_SUBS)];
    if (!sub->used || sub->dead || sub == other) {
        return NULL;
    }
    return sub;
}

static void subscribe(random_state *state);

static void act(random_sub *sub, const int *actions, size_t count)
{
    random_state *state = sub->state;
    if (state->actions == RANDOM_ACTIONS || random_next(state, 100) >= 20) {
        return;
    }
    state->actions++;
    switch (actions[random_next(state, count)]) {
        case ACTION_PUBLISH:
            publish(state, 1);
            break;
        case ACTION_PUBLISH_BATCH:
            publish(state, 2 + random_next(state, 3));
            break;
        case ACTION_PUBLISH_SIDE:
            publish_side(state, 1 + random_next(state, 4));
            break;
        case ACTION_NOTIFY:
            notify(state);
            break;
        case ACTION_TERMINATE:
            terminate(state, random_next(state, 2));
            break;
        case ACTION_REOPEN:
            if (state->subject_calls) {
                assert_int_equal(evio_rx_subject_reopen(state->subject), -1);
                assert_int_equal(errno, EBUSY);
            }
            break;
        case ACTION_SUBSCRIBE:
            subscribe(state);
            break;
        case ACTION_CANCEL:
            sub_cancel(sub);
            break;
        case ACTION_RESTART:
            sub_restart(sub);
            break;
        case ACTION_FREE:
            sub_free(sub);
            break;
        case ACTION_OTHER: {
            random_sub *other = pick(state, sub);
            if (!other) {
                break;
            }
            uint64_t kind = random_next(state, 3);
            if (kind == 0) {
                sub_cancel(other);
            } else if (kind == 1) {
                sub_restart(other);
            } else {
                sub_free(other);
            }
            break;
        }
        default:
            fail();
    }
}

static const int tap_actions[] = {
    ACTION_PUBLISH, ACTION_PUBLISH_BATCH, ACTION_PUBLISH_SIDE, ACTION_NOTIFY, ACTION_TERMINATE,
    ACTION_REOPEN,
};

static const int next_actions[] = {
    ACTION_PUBLISH, ACTION_PUBLISH_BATCH, ACTION_PUBLISH_SIDE, ACTION_NOTIFY, ACTION_TERMINATE,
    ACTION_REOPEN, ACTION_SUBSCRIBE, ACTION_CANCEL, ACTION_RESTART, ACTION_FREE, ACTION_OTHER,
};

static const int end_actions[] = {
    ACTION_PUBLISH, ACTION_PUBLISH_SIDE, ACTION_NOTIFY, ACTION_TERMINATE, ACTION_SUBSCRIBE,
    ACTION_RESTART, ACTION_FREE, ACTION_OTHER,
};

static int tap_batch(void *ctx, void *out, const void *values, size_t count)
{
    random_sub *sub = ctx;
    enter(sub);
    int error = 0;
    size_t built = 0;
    for (; built < count && !error; ++built) {
        uint64_t value = box_value((uint64_t *const *)values + built);
        assert_true(value > sub->last_id);
        sub->last_id = value;
        list_push(&sub->tap, value);
        error = box_new(sub->state, (uint64_t **)out + built, value);
    }
    if (error) {
        boxes_destroy(sub->state, out, built - 1);
    }
    act(sub, tap_actions, sizeof(tap_actions) / sizeof(*tap_actions));
    leave(sub);
    return error;
}

static int side_tap(void *ctx, void *out, const void *values, size_t count)
{
    const random_stage *stage = ctx;
    random_sub *sub = stage->sub;
    enter(sub);
    int error = 0;
    size_t built = 0;
    for (; built < count && !error; ++built) {
        uint64_t value = box_value((uint64_t *const *)values + built);
        list_push(&sub->tap, ((stage->index + 1) << 32) | value);
        error = box_new(sub->state, (uint64_t **)out + built, value);
    }
    if (error) {
        boxes_destroy(sub->state, out, built - 1);
    }
    act(sub, tap_actions, sizeof(tap_actions) / sizeof(*tap_actions));
    leave(sub);
    return error;
}

static void observer_next(void *ctx, const void *value)
{
    random_sub *sub = ctx;
    enter(sub);
    list_push(&sub->out, box_value(value));
    act(sub, next_actions, sizeof(next_actions) / sizeof(*next_actions));
    leave(sub);
}

static void observer_end(random_sub *sub, int end, int error)
{
    enter(sub);
    assert_true(evio_rx_subscription_is_closed(sub->subscription));
    sub->end = end;
    sub->error = error;
    check_run(sub);
    sub->open = false;
    act(sub, end_actions, sizeof(end_actions) / sizeof(*end_actions));
    leave(sub);
}

static void observer_error(void *ctx, int error)
{
    observer_end(ctx, END_ERROR, error);
}

static void observer_complete(void *ctx)
{
    observer_end(ctx, END_COMPLETE, 0);
}

static int map_add(void *ctx, void *out, const void *value)
{
    const random_stage *stage = ctx;
    return box_new(stage->state, out, box_value(value) + stage->arg);
}

static bool filter_mod(void *ctx, const void *value)
{
    const random_stage *stage = ctx;
    return box_value(value) % stage->arg;
}

static int scan_sum(void *ctx, void *out, const void *accumulator, const void *value)
{
    const random_stage *stage = ctx;
    return box_new(stage->state, out, box_value(accumulator) + box_value(value));
}

static bool equal_half(void *ctx, const void *previous, const void *value)
{
    return box_value(previous) / 2 == box_value(value) / 2;
}

static int triple_batch(void *ctx, void *out, const void *values, size_t count)
{
    const random_stage *stage = ctx;
    for (size_t i = 0; i < count; ++i) {
        uint64_t value = box_value((uint64_t *const *)values + i);
        int error = box_new(stage->state, (uint64_t **)out + i, value * 3);
        if (error) {
            boxes_destroy(stage->state, out, i);
            return error;
        }
    }
    return 0;
}

static int keep_batch(void *ctx, void *out, size_t *kept, const void *values, size_t count)
{
    const random_stage *stage = ctx;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t value = box_value((uint64_t *const *)values + i);
        if (!(value % stage->arg)) {
            continue;
        }
        int error = box_new(stage->state, (uint64_t **)out + n, value);
        if (error) {
            boxes_destroy(stage->state, out, n);
            return error;
        }
        n++;
    }
    *kept = n;
    return 0;
}

static int fail_mod(void *ctx, void *out, const void *value)
{
    const random_stage *stage = ctx;
    if (!(box_value(value) % stage->arg)) {
        return EDOM;
    }
    return box_new(stage->state, out, box_value(value));
}

static int pair_values(void *ctx, void *out, const void *const *values, size_t count)
{
    const random_stage *stage = ctx;
    return box_new(stage->state, out, (box_value(values[0]) * PAIR_BASE) + box_value(values[1]));
}

static evio_rx_observable *array_of(random_state *state, uint64_t first)
{
    uint64_t values[] = { first, first + 1 };
    uint64_t *boxes[] = { &values[0], &values[1] };
    return evio_rx_from_array(&state->type, boxes, 2);
}

static evio_rx_observable *project_pair(void *ctx, const void *value)
{
    return array_of(ctx, box_value(value));
}

static evio_rx_observable *project_same(void *ctx, const void *value)
{
    return evio_rx_observable_ref(ctx);
}

static void observable_destroy(void *data)
{
    evio_rx_observable_free(data);
}

static evio_rx_observable *fan_in_new(random_stage *stage, evio_rx_observable *input)
{
    random_state *state = stage->state;
    evio_rx_observable *side = stage->arg ? state->side_user : state->side_subject;
    side = evio_rx_map_batch(side, &state->type, side_tap, (evio_rx_capture) { .data = stage });
    assert_non_null(side);
    evio_rx_observable *inputs[] = { input, side };
    evio_rx_combiner pair = {
        .type = &state->type,
        .cb = pair_values,
        .capture = { .data = stage },
    };
    evio_rx_observable *result;
    if (stage->kind == STAGE_MERGE) {
        result = evio_rx_merge(inputs, 2);
    } else if (stage->kind == STAGE_ZIP) {
        result = evio_rx_zip(inputs, 2, pair, RANDOM_QUEUE);
    } else {
        result = evio_rx_combine_latest(inputs, 2, pair);
    }
    evio_rx_observable_free(side);
    return result;
}

static evio_rx_observable *stage_new(random_stage *stage, evio_rx_observable *input)
{
    random_state *state = stage->state;
    evio_rx_capture capture = { .data = stage };
    evio_rx_project pair = {
        .type = &state->type,
        .cb = project_pair,
        .capture = { .data = state },
    };
    switch (stage->kind) {
        case STAGE_MAP:
            return evio_rx_map(input, &state->type, map_add, capture);
        case STAGE_FILTER:
            return evio_rx_filter(input, filter_mod, capture);
        case STAGE_SCAN: {
            uint64_t zero = 0;
            uint64_t *seed = &zero;
            return evio_rx_scan(input, &state->type, &seed, scan_sum, capture);
        }
        case STAGE_DISTINCT:
            return evio_rx_distinct_until_changed(input, equal_half, capture);
        case STAGE_MAP_BATCH:
            return evio_rx_map_batch(input, &state->type, triple_batch, capture);
        case STAGE_FILTER_BATCH:
            return evio_rx_filter_batch(input, keep_batch, capture);
        case STAGE_SWITCH:
            return evio_rx_switch_map(input, pair);
        case STAGE_SWITCH_SAME: {
            evio_rx_project same = {
                .type = &state->type,
                .cb = project_same,
                .capture = { .data = array_of(state, 1000), .destroy = observable_destroy },
            };
            return evio_rx_switch_map(input, same);
        }
        case STAGE_CONCAT:
            return evio_rx_concat_map(input, pair, 4);
        case STAGE_MERGE_MAP:
            return evio_rx_merge_map(input, pair, 2, 4);
        case STAGE_FAIL:
            return evio_rx_map(input, &state->type, fail_mod, capture);
        case STAGE_TAKE_UNTIL:
            return evio_rx_take_until(input, state->stop);
        case STAGE_MERGE:
        case STAGE_ZIP:
        case STAGE_LATEST:
            return fan_in_new(stage, input);
        default:
            fail();
    }
    return NULL;
}

static evio_rx_observable *then(evio_rx_observable *input, evio_rx_observable *output)
{
    assert_non_null(output);
    evio_rx_observable_free(input);
    return output;
}

static void subscribe(random_state *state)
{
    random_sub *sub = NULL;
    for (size_t i = 0; i < RANDOM_SUBS && !sub; ++i) {
        sub = state->subs[i].used ? NULL : &state->subs[i];
    }
    if (!sub) {
        return;
    }
    bool failures = state->failures;
    state->failures = false;
    *sub = (random_sub) {
        .state = state,
        .used = true,
        .stage_count = random_next(state, RANDOM_STAGES + 1),
        .tap = sub->tap,
        .out = sub->out,
    };
    evio_rx_observable *source = evio_rx_observable_ref(state->source);
    source = then(source, evio_rx_map_batch(source, &state->type, tap_batch,
                                            (evio_rx_capture) { .data = sub }));
    for (size_t i = 0; i < sub->stage_count; ++i) {
        random_stage *stage = &sub->stages[i];
        stage->state = state;
        stage->sub = sub;
        stage->index = i;
        stage->kind = (int)random_next(state, STAGE_COUNT);
        if (stage->kind == STAGE_FAIL) {
            stage->arg = 23 + random_next(state, 20);
        } else if (stage->kind >= STAGE_MERGE) {
            stage->arg = random_next(state, 2);
        } else {
            stage->arg = 2 + random_next(state, 3);
        }
        sub->take_until |= stage->kind == STAGE_TAKE_UNTIL;
        sub->early_end |= stage->kind == STAGE_ZIP || stage->kind == STAGE_LATEST;
        source = then(source, stage_new(stage, source));
    }
    state->failures = failures;
    evio_rx_observer observer = {
        .next = observer_next,
        .error = observer_error,
        .complete = observer_complete,
        .ctx = sub,
    };
    sub->subscription = evio_rx_subscription_new(state->loop, source, observer, RANDOM_QUEUE);
    evio_rx_observable_free(source);
    if (!sub->subscription) {
        assert_true(failures);
        assert_int_equal(errno, ENOMEM);
        sub->used = false;
        return;
    }
    run_begin(sub);
    evio_rx_subscription_start(sub->subscription);
}

static void check_quiescent(random_state *state)
{
    for (size_t i = 0; i < RANDOM_SUBS; ++i) {
        random_sub *sub = &state->subs[i];
        if (sub->dead) {
            sub->used = false;
            sub->dead = false;
        }
        if (!sub->used) {
            continue;
        }
        assert_int_equal(evio_rx_subscription_is_closed(sub->subscription), !sub->open);
        if (sub->open) {
            check_run(sub);
        }
    }
}

static void step(random_state *state)
{
    uint64_t choice = random_next(state, 100);
    if (choice < 28) {
        publish(state, 1);
    } else if (choice < 35) {
        publish_side(state, 1 + random_next(state, 4));
    } else if (choice < 55) {
        publish(state, 1 + random_next(state, 8));
    } else if (choice < 59) {
        terminate(state, true);
    } else if (choice < 62) {
        terminate(state, false);
    } else if (choice < 68) {
        assert_int_equal(evio_rx_subject_reopen(state->subject), 0);
        assert_int_equal(evio_rx_subject_reopen(state->side), 0);
        state->closed = false;
        state->completed = false;
        state->side_closed = false;
        state->side_failed = false;
        state->side_error = 0;
        for (size_t i = 0; i < RANDOM_SUBS; ++i) {
            random_sub *closed = &state->subs[i];
            if (closed->used && !closed->dead && !closed->open && random_next(state, 2)) {
                sub_restart(closed);
            }
        }
    } else if (choice < 78) {
        subscribe(state);
    } else if (choice < 82) {
        notify(state);
    } else {
        random_sub *sub = pick(state, NULL);
        if (!sub) {
            return;
        }
        if (choice < 88) {
            sub_restart(sub);
        } else if (choice < 94) {
            sub_cancel(sub);
        } else {
            sub_free(sub);
        }
    }
}

static void random_run(uint64_t seed, bool failures)
{
    random_state state = {
        .loop = evio_loop_new(EVIO_FLAG_NONE),
        .rng = seed * 0x9e3779b97f4a7c15ULL,
        .next_id = 1,
        .side_next_id = SIDE_FIRST,
    };
    state.type = (evio_rx_type) {
        .size = sizeof(uint64_t *),
        .alignment = alignof(uint64_t *),
        .copy = box_copy,
        .destroy = box_destroy,
        .ctx = &state,
    };
    state.subject = evio_rx_subject_new(state.loop, &state.type, RANDOM_QUEUE);
    state.notifier = evio_rx_subject_new(state.loop, &state.type, RANDOM_QUEUE);
    state.source = evio_rx_from_subject(state.subject);
    state.stop = evio_rx_from_subject(state.notifier);
    state.side = evio_rx_subject_new(state.loop, &state.type, RANDOM_QUEUE);
    state.side_subject = evio_rx_from_subject(state.side);
    evio_rx_source side = {
        .prepare = side_prepare,
        .start = side_start,
        .stop = side_stop,
        .destroy = side_destroy,
    };
    state.side_user = evio_rx_observable_new(&state.type, side,
                                             (evio_rx_capture) { .data = &state });
    assert_non_null(state.source);
    assert_non_null(state.stop);
    assert_non_null(state.side_subject);
    assert_non_null(state.side_user);
    evio_rx_observer counter = { .next = count_notification, .ctx = &state };
    state.notifications_counter = evio_rx_subscription_new(state.loop, state.stop, counter,
                                                           RANDOM_QUEUE);
    assert_non_null(state.notifications_counter);
    evio_rx_subscription_start(state.notifications_counter);
    state.failures = failures;

    for (size_t i = 0; i < RANDOM_STEPS; ++i) {
        step(&state);
        check_quiescent(&state);
    }
    terminate(&state, true);
    check_quiescent(&state);
    for (size_t i = 0; i < RANDOM_SUBS; ++i) {
        random_sub *sub = &state.subs[i];
        if (sub->used) {
            sub_free(sub);
        }
        list_free(&sub->tap);
        list_free(&sub->out);
    }
    evio_rx_subscription_free(state.notifications_counter);
    evio_rx_observable_free(state.source);
    evio_rx_observable_free(state.stop);
    evio_rx_observable_free(state.side_subject);
    evio_rx_observable_free(state.side_user);
    evio_rx_subject_free(state.subject);
    evio_rx_subject_free(state.notifier);
    evio_rx_subject_free(state.side);
    evio_free(state.side_runs.items);
    assert_int_equal(state.side_live, 0);
    assert_int_equal(state.boxes, 0);
    assert_int_equal(state.loop->refcount, 0);
    evio_loop_free(state.loop);
}

TEST(test_evio_rx_random_reentry_matches_model)
{
    for (uint64_t seed = 1; seed <= RANDOM_SEEDS; ++seed) {
        random_run(seed, false);
    }
}

TEST(test_evio_rx_random_copy_failures_release_values)
{
    for (uint64_t seed = 1; seed <= RANDOM_SEEDS; ++seed) {
        random_run(seed, true);
    }
}
