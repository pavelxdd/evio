#include <errno.h>

#include "evio_rx_node.h"
#include "evio_rx_buffer.h"
#include "evio_alloc.h"

typedef struct {
    evio_rx_observable base;
    evio_rx_observable *source;
    evio_time duration;
    size_t capacity;
} evio_rx_time_observable;

typedef struct {
    evio_rx_node base;
    evio_rx_node *input;
    evio_rx_timer_state timer;
    evio_rx_buffer value;
    bool receiving;
} evio_rx_time_node;

static int time_init(evio_rx_node *base)
{
    evio_rx_time_node *node = (evio_rx_time_node *)base;
    evio_rx_time_observable *source = (evio_rx_time_observable *)base->source;
    evio_rx_timer_init(&node->timer, base);
    node->input = evio_rx_node_new(base, source->source, 0);
    return node->input ? 0 : errno;
}

static int debounce_init(evio_rx_node *base)
{
    evio_rx_time_node *node = (evio_rx_time_node *)base;
    int error = evio_rx_buffer_init(&node->value, base->source->type, 1, base->root);
    if (!error) {
        error = time_init(base);
    }
    if (error) {
        evio_rx_buffer_fini(&node->value, base->root);
    }
    return error;
}

static void time_start(evio_rx_node *base)
{
    evio_rx_time_node *node = (evio_rx_time_node *)base;
    evio_rx_node_start(node->input);
}

static void time_stop(evio_rx_node *base)
{
    evio_rx_time_node *node = (evio_rx_time_node *)base;
    evio_rx_timer_stop(&node->timer);
    if (!node->receiving) {
        evio_rx_buffer_clear(&node->value);
    }
}

static void time_fini(evio_rx_node *base)
{
    evio_rx_time_node *node = (evio_rx_time_node *)base;
    evio_rx_buffer_fini(&node->value, base->root);
}

static bool is_due(const evio_rx_time_node *node)
{
    return evio_get_time(node->base.root->loop) >= node->timer.deadline;
}

static void debounce_emit(evio_rx_time_node *node)
{
    evio_rx_timer_stop(&node->timer);
    if (node->value.count) {
        evio_rx_node_next(&node->base, node->value.data);
        evio_rx_buffer_clear(&node->value);
    }
}

// An armed timer is moved only when it ticks before the deadline.
static int debounce_arm(evio_rx_time_node *node, evio_time duration)
{
    if (!node->timer.armed) {
        return evio_rx_timer_start(&node->timer, duration);
    }
    return evio_rx_deadline(node->base.root->loop, duration, &node->timer.deadline);
}

static void debounce_tick(evio_rx_node *base)
{
    evio_rx_time_node *node = (evio_rx_time_node *)base;
    if (!node->value.count) {
        return;
    }
    if (!is_due(node)) {
        evio_time now = evio_get_time(base->root->loop);
        (void)evio_rx_timer_start(&node->timer, node->timer.deadline - now);
        return;
    }
    node->receiving = true;
    debounce_emit(node);
    node->receiving = false;
    if (base->closed) {
        evio_rx_buffer_clear(&node->value);
    }
}

static void debounce_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_time_node *node = (evio_rx_time_node *)base;
    evio_rx_time_observable *source = (evio_rx_time_observable *)base->source;
    node->receiving = true;
    if (node->value.count && is_due(node)) {
        debounce_emit(node);
    }
    if (!base->closed) {
        evio_rx_buffer_clear(&node->value);
        int error = evio_rx_buffer_push(&node->value, value);
        if (!error && !base->closed) {
            error = debounce_arm(node, source->duration);
        }
        if (error) {
            evio_rx_fail(base->root, error);
        }
    }
    node->receiving = false;
    if (base->closed) {
        evio_rx_buffer_clear(&node->value);
    }
}

static void debounce_end(evio_rx_node *base, evio_rx_node *input, int error)
{
    evio_rx_time_node *node = (evio_rx_time_node *)base;
    if (!error) {
        node->receiving = true;
        debounce_emit(node);
        node->receiving = false;
    }
    evio_rx_node_finish(base, error);
    evio_rx_buffer_clear(&node->value);
}

static void time_free(evio_rx_observable *base)
{
    evio_rx_time_observable *source = (evio_rx_time_observable *)base;
    evio_rx_observable_free(source->source);
    evio_free(source);
}

static const evio_rx_ops debounce_ops = {
    .node_size = sizeof(evio_rx_time_node),
    .init = debounce_init,
    .start = time_start,
    .stop = time_stop,
    .fini = time_fini,
    .next = debounce_next,
    .end = debounce_end,
    .tick = debounce_tick,
    .free = time_free,
};

static evio_rx_observable *time_new(evio_rx_observable *source, evio_time duration,
                                    const evio_rx_ops *ops)
{
    if (!source) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_time_observable *result = evio_calloc(1, sizeof(*result));
    evio_rx_observable_init(&result->base, ops, source->type, source->loop, (evio_rx_capture) {0});
    result->source = evio_rx_observable_ref(source);
    result->duration = duration;
    return &result->base;
}

evio_rx_observable *evio_rx_debounce_time(evio_rx_observable *source, evio_time duration)
{
    if (!duration) {
        errno = EINVAL;
        return NULL;
    }
    return time_new(source, duration, &debounce_ops);
}

typedef struct {
    evio_rx_node base;
    evio_rx_node *input;
    evio_time deadline;
} evio_rx_throttle_node;

static int throttle_init(evio_rx_node *base)
{
    evio_rx_throttle_node *node = (evio_rx_throttle_node *)base;
    evio_rx_time_observable *source = (evio_rx_time_observable *)base->source;
    node->input = evio_rx_node_new(base, source->source, 0);
    return node->input ? 0 : errno;
}

static void throttle_start(evio_rx_node *base)
{
    evio_rx_throttle_node *node = (evio_rx_throttle_node *)base;
    evio_rx_node_start(node->input);
}

static void throttle_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_throttle_node *node = (evio_rx_throttle_node *)base;
    evio_rx_time_observable *source = (evio_rx_time_observable *)base->source;
    evio_loop *loop = base->root->loop;
    if (evio_get_time(loop) < node->deadline) {
        return;
    }
    int error = evio_rx_deadline(loop, source->duration, &node->deadline);
    if (error) {
        evio_rx_fail(base->root, error);
        return;
    }
    evio_rx_node_next(base, value);
}

static void throttle_reset(evio_rx_node *base)
{
    evio_rx_throttle_node *node = (evio_rx_throttle_node *)base;
    node->deadline = 0;
}

static const evio_rx_ops throttle_ops = {
    .node_size = sizeof(evio_rx_throttle_node),
    .init = throttle_init,
    .start = throttle_start,
    .reset = throttle_reset,
    .next = throttle_next,
    .end = evio_rx_forward_end,
    .free = time_free,
};

evio_rx_observable *evio_rx_throttle_time(evio_rx_observable *source, evio_time duration)
{
    if (!duration) {
        errno = EINVAL;
        return NULL;
    }
    return time_new(source, duration, &throttle_ops);
}

typedef struct {
    evio_rx_time_node base;
    evio_rx_ring queue;
    evio_time *deadlines;       /**< Deadlines of queued values, by ring slot. */
    bool completed;
} evio_rx_delay_node;

static void delay_fini(evio_rx_node *base)
{
    evio_rx_delay_node *node = (evio_rx_delay_node *)base;
    evio_rx_free(base->root, node->deadlines, node->queue.slots * sizeof(*node->deadlines));
    evio_rx_ring_fini(&node->queue, base->root);
}

static int delay_init(evio_rx_node *base)
{
    evio_rx_delay_node *node = (evio_rx_delay_node *)base;
    evio_rx_time_observable *source = (evio_rx_time_observable *)base->source;
    int error = evio_rx_ring_init(&node->queue, base->source->type, source->capacity, base->root);
    if (error) {
        return error;
    }
    node->deadlines = evio_rx_alloc(base->root, node->queue.slots * sizeof(*node->deadlines));
    error = time_init(base);
    if (error) {
        delay_fini(base);
    }
    return error;
}

static void delay_stop(evio_rx_node *base)
{
    evio_rx_delay_node *node = (evio_rx_delay_node *)base;
    time_stop(base);
    if (!node->base.receiving) {
        evio_rx_ring_clear(&node->queue);
    }
}

static int delay_arm(evio_rx_delay_node *node)
{
    evio_time now = evio_get_time(node->base.base.root->loop);
    evio_time deadline = node->deadlines[node->queue.head];
    return evio_rx_timer_start(&node->base.timer, deadline > now ? deadline - now : 0);
}

static void delay_done(evio_rx_delay_node *node)
{
    node->base.receiving = false;
    if (node->base.base.closed) {
        evio_rx_ring_clear(&node->queue);
    }
}

static void delay_tick(evio_rx_node *base)
{
    evio_rx_delay_node *node = (evio_rx_delay_node *)base;
    node->base.receiving = true;
    evio_rx_timer_stop(&node->base.timer);
    // One value per tick lets queued upstream errors preempt the tail.
    if (node->queue.count &&
        node->deadlines[node->queue.head] <= evio_get_time(base->root->loop)) {
        void *value = evio_rx_ring_shift(&node->queue);
        evio_rx_node_next(base, value);
        evio_rx_value_destroy(node->queue.type, value);
        if (base->closed) {
            delay_done(node);
            return;
        }
    }
    if (node->queue.count) {
        int error = delay_arm(node);
        if (error) {
            evio_rx_fail(base->root, error);
        }
    } else if (node->completed) {
        evio_rx_node_finish(base, 0);
    }
    delay_done(node);
}

static void delay_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_delay_node *node = (evio_rx_delay_node *)base;
    evio_rx_time_observable *source = (evio_rx_time_observable *)base->source;
    node->base.receiving = true;
    evio_time deadline;
    size_t slot = evio_rx_ring_back(&node->queue);
    int error = evio_rx_deadline(base->root->loop, source->duration, &deadline);
    if (!error) {
        error = evio_rx_ring_push(&node->queue, value);
    }
    if (!error) {
        node->deadlines[slot] = deadline;
        if (!base->closed && node->queue.count == 1) {
            error = delay_arm(node);
        }
    }
    if (error) {
        evio_rx_fail(base->root, error);
    }
    delay_done(node);
}

static void delay_end(evio_rx_node *base, evio_rx_node *input, int error)
{
    evio_rx_delay_node *node = (evio_rx_delay_node *)base;
    node->completed = !error;
    if (error || !node->queue.count) {
        evio_rx_node_finish(base, error);
    }
}

static void delay_reset(evio_rx_node *base)
{
    evio_rx_delay_node *node = (evio_rx_delay_node *)base;
    evio_rx_ring_clear(&node->queue);
    node->completed = false;
}

static const evio_rx_ops delay_ops = {
    .node_size = sizeof(evio_rx_delay_node),
    .init = delay_init,
    .start = time_start,
    .stop = delay_stop,
    .reset = delay_reset,
    .fini = delay_fini,
    .next = delay_next,
    .end = delay_end,
    .tick = delay_tick,
    .free = time_free,
};

evio_rx_observable *evio_rx_delay(evio_rx_observable *source, evio_time duration, size_t capacity)
{
    if (!capacity) {
        errno = EINVAL;
        return NULL;
    }
    int error = source ? evio_rx_ring_check(source->type, capacity) : 0;
    if (!error && capacity >= SIZE_MAX / sizeof(evio_time)) {
        error = EOVERFLOW;
    }
    if (error) {
        errno = error;
        return NULL;
    }
    evio_rx_time_observable *result =
        (evio_rx_time_observable *)time_new(source, duration, &delay_ops);
    if (!result) {
        return NULL;
    }
    result->capacity = capacity;
    return &result->base;
}

static void timeout_start(evio_rx_node *base)
{
    evio_rx_time_node *node = (evio_rx_time_node *)base;
    evio_rx_time_observable *source = (evio_rx_time_observable *)base->source;
    int error = evio_rx_timer_start(&node->timer, source->duration);
    if (error) {
        evio_rx_fail(base->root, error);
        return;
    }
    time_start(base);
}

static void timeout_tick(evio_rx_node *base)
{
    evio_rx_fail(base->root, ETIMEDOUT);
}

static void timeout_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_time_node *node = (evio_rx_time_node *)base;
    evio_rx_time_observable *source = (evio_rx_time_observable *)base->source;
    if (is_due(node)) {
        evio_rx_fail(base->root, ETIMEDOUT);
        return;
    }
    evio_rx_node_next(base, value);
    if (base->closed) {
        return;
    }
    int error = evio_rx_timer_start(&node->timer, source->duration);
    if (error) {
        evio_rx_fail(base->root, error);
    }
}

static void timeout_end(evio_rx_node *base, evio_rx_node *input, int error)
{
    evio_rx_time_node *node = (evio_rx_time_node *)base;
    if (is_due(node)) {
        evio_rx_fail(base->root, ETIMEDOUT);
        return;
    }
    evio_rx_node_finish(base, error);
}

static const evio_rx_ops timeout_ops = {
    .node_size = sizeof(evio_rx_time_node),
    .init = time_init,
    .start = timeout_start,
    .stop = time_stop,
    .next = timeout_next,
    .end = timeout_end,
    .tick = timeout_tick,
    .free = time_free,
};

evio_rx_observable *evio_rx_timeout(evio_rx_observable *source, evio_time duration)
{
    if (!duration) {
        errno = EINVAL;
        return NULL;
    }
    return time_new(source, duration, &timeout_ops);
}
