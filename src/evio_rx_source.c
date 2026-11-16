#include <errno.h>

#include "evio_rx_node.h"
#include "evio_rx_buffer.h"
#include "evio_alloc.h"

typedef struct {
    evio_rx_observable base;
    evio_rx_source source;
} evio_rx_user;

static int user_init(evio_rx_node *base)
{
    evio_rx_user *source = (evio_rx_user *)base->source;
    evio_rx_emitter *emitter = (evio_rx_emitter *)base;
    int error = source->source.prepare(source->base.capture.data, base->root->loop,
                                       emitter, &emitter->state);
    emitter->prepared = !error;
    return error;
}

// A restarted run prepares its state at start.
static void user_start(evio_rx_node *base)
{
    evio_rx_user *source = (evio_rx_user *)base->source;
    evio_rx_emitter *emitter = (evio_rx_emitter *)base;
    if (!emitter->prepared) {
        int error = user_init(base);
        if (error) {
            evio_rx_fail(base->root, error);
            return;
        }
    }
    source->source.start(emitter, emitter->state);
}

static void user_stop(evio_rx_node *base)
{
    evio_rx_user *source = (evio_rx_user *)base->source;
    evio_rx_emitter *emitter = (evio_rx_emitter *)base;
    if (emitter->prepared && source->source.stop) {
        source->source.stop(emitter->state);
    }
}

static void user_fini(evio_rx_node *base)
{
    evio_rx_user *source = (evio_rx_user *)base->source;
    evio_rx_emitter *emitter = (evio_rx_emitter *)base;
    if (emitter->prepared && source->source.destroy) {
        source->source.destroy(emitter->state);
    }
    emitter->state = NULL;
    emitter->prepared = false;
}

static void source_free(evio_rx_observable *source)
{
    evio_free(source);
}

static const evio_rx_ops user_ops = {
    .node_size = sizeof(evio_rx_emitter),
    .init = user_init,
    .start = user_start,
    .stop = user_stop,
    .reset = user_fini,
    .fini = user_fini,
    .free = source_free,
};

evio_rx_observable *evio_rx_observable_new(const evio_rx_type *type, evio_rx_source source,
                                           evio_rx_capture capture)
{
    if (evio_rx_type_check(type) || !source.prepare || !source.start) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_user *result = evio_calloc(1, sizeof(*result));
    evio_rx_observable_init(&result->base, &user_ops, type, NULL, capture);
    result->source = source;
    return &result->base;
}

void evio_rx_emitter_invoke(evio_rx_emitter *emitter, evio_rx_source_cb cb, void *data)
{
    evio_rx_node *node = &emitter->base;
    evio_rx_subscription *root = node->root;
    if (node->input_closed) {
        return;
    }
    evio_rx_enter(root);
    evio_rx_node *producer = root->producer;
    if (!producer && !root->depth) {
        root->producer = node;
    }
    node->active++;
    cb(emitter, emitter->state, data);
    node->active--;
    root->producer = producer;
    evio_rx_leave(root);
}

bool evio_rx_emitter_next(evio_rx_emitter *emitter, const void *value)
{
    return evio_rx_emit(&emitter->base, value);
}

void evio_rx_emitter_error(evio_rx_emitter *emitter, int error)
{
    evio_rx_emit_end(&emitter->base, error > 0 ? error : EINVAL);
}

void evio_rx_emitter_complete(evio_rx_emitter *emitter)
{
    evio_rx_emit_end(&emitter->base, 0);
}

const evio_rx_type evio_rx_uint64_type = {
    .size = sizeof(uint64_t), .alignment = alignof(uint64_t),
};

typedef struct {
    evio_rx_observable base;
    evio_time after;
    evio_time period;
} evio_rx_timer_observable;

typedef struct {
    evio_rx_node base;
    evio_rx_timer_state timer;
    uint64_t counter;
} evio_rx_timer_node;

static int timer_init(evio_rx_node *base)
{
    evio_rx_timer_node *node = (evio_rx_timer_node *)base;
    evio_rx_timer_init(&node->timer, base);
    return 0;
}

static void timer_start(evio_rx_node *base)
{
    evio_rx_timer_node *node = (evio_rx_timer_node *)base;
    evio_rx_timer_observable *source = (evio_rx_timer_observable *)base->source;
    int error = evio_rx_timer_start(&node->timer, source->after);
    if (error) {
        evio_rx_fail(base->root, error);
    }
}

static void timer_stop(evio_rx_node *base)
{
    evio_rx_timer_node *node = (evio_rx_timer_node *)base;
    evio_rx_timer_stop(&node->timer);
}

static void timer_reset(evio_rx_node *base)
{
    evio_rx_timer_node *node = (evio_rx_timer_node *)base;
    node->counter = 0;
}

static void timer_tick(evio_rx_node *base)
{
    evio_rx_timer_node *node = (evio_rx_timer_node *)base;
    evio_rx_timer_observable *source = (evio_rx_timer_observable *)base->source;
    evio_rx_node_next(base, &node->counter);
    if (base->closed) {
        return;
    }
    if (!source->period) {
        evio_rx_node_finish(base, 0);
        return;
    }
    if (node->counter == UINT64_MAX) {
        evio_rx_fail(base->root, EOVERFLOW);
        return;
    }
    node->counter++;
    int error = evio_rx_timer_start(&node->timer, source->period);
    if (error) {
        evio_rx_fail(base->root, error);
    }
}

static const evio_rx_ops timer_ops = {
    .node_size = sizeof(evio_rx_timer_node),
    .init = timer_init,
    .start = timer_start,
    .stop = timer_stop,
    .reset = timer_reset,
    .tick = timer_tick,
    .free = source_free,
};

evio_rx_observable *evio_rx_timer(evio_time after)
{
    evio_rx_timer_observable *source = evio_calloc(1, sizeof(*source));
    evio_rx_observable_init(&source->base, &timer_ops, &evio_rx_uint64_type,
                            NULL, (evio_rx_capture) {0});
    source->after = after;
    return &source->base;
}

evio_rx_observable *evio_rx_interval(evio_time after, evio_time period)
{
    if (!period) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_timer_observable *source = (evio_rx_timer_observable *)evio_rx_timer(after);
    source->period = period;
    return &source->base;
}

const evio_rx_type evio_rx_poll_event_type = {
    .size = sizeof(evio_rx_poll_event), .alignment = alignof(evio_rx_poll_event),
};

typedef struct {
    evio_rx_observable base;
    int fd;
    evio_mask events;
} evio_rx_poll_observable;

typedef struct {
    evio_rx_node base;
    evio_poll watcher;
} evio_rx_poll_node;

static void poll_cb(evio_loop *loop, evio_base *watcher, evio_mask emask)
{
    evio_rx_poll_node *node = watcher->data;
    if (emask & EVIO_ERROR) {
        evio_rx_emit_end(&node->base, EIO);
        return;
    }
    evio_rx_poll_event event = {
        .fd = node->watcher.fd,
        .events = emask & (EVIO_READ | EVIO_WRITE),
    };
    if (event.events) {
        (void)evio_rx_emit(&node->base, &event);
    }
}

static int poll_init(evio_rx_node *base)
{
    evio_rx_poll_node *node = (evio_rx_poll_node *)base;
    evio_rx_poll_observable *source = (evio_rx_poll_observable *)base->source;
    evio_poll_init(&node->watcher, poll_cb, source->fd, source->events);
    node->watcher.data = node;
    return 0;
}

static void poll_start(evio_rx_node *base)
{
    evio_rx_poll_node *node = (evio_rx_poll_node *)base;
    evio_poll_start(base->root->loop, &node->watcher);
}

static void poll_stop(evio_rx_node *base)
{
    evio_rx_poll_node *node = (evio_rx_poll_node *)base;
    evio_poll_stop(base->root->loop, &node->watcher);
}

static const evio_rx_ops poll_ops = {
    .node_size = sizeof(evio_rx_poll_node),
    .init = poll_init,
    .start = poll_start,
    .stop = poll_stop,
    .free = source_free,
};

evio_rx_observable *evio_rx_from_poll(int fd, evio_mask events)
{
    if (fd < 0 || !events || (events & ~(EVIO_READ | EVIO_WRITE))) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_poll_observable *source = evio_calloc(1, sizeof(*source));
    evio_rx_observable_init(&source->base, &poll_ops, &evio_rx_poll_event_type,
                            NULL, (evio_rx_capture) {0});
    source->fd = fd;
    source->events = events;
    return &source->base;
}

typedef struct {
    evio_rx_observable base;
    evio_rx_buffer items;
} evio_rx_array;

static void array_start(evio_rx_node *node)
{
    evio_rx_array *source = (evio_rx_array *)node->source;
    const char *values = source->items.data;
    size_t size = source->base.type->size;
    for (size_t i = 0; i < source->items.count; i += EVIO_RX_BATCH) {
        size_t count = source->items.count - i;
        if (count > EVIO_RX_BATCH) {
            count = EVIO_RX_BATCH;
        }
        if (!evio_rx_emit_batch(node, values + (i * size), count)) {
            return;
        }
    }
    evio_rx_emit_end(node, 0);
}

static void array_free(evio_rx_observable *base)
{
    evio_rx_array *source = (evio_rx_array *)base;
    evio_rx_buffer_fini(&source->items, NULL);
    evio_free(source);
}

static const evio_rx_ops array_ops = {
    .node_size = sizeof(evio_rx_node),
    .start = array_start,
    .free = array_free,
};

evio_rx_observable *evio_rx_from_array(const evio_rx_type *type, const void *items, size_t count)
{
    int error = evio_rx_type_check(type);
    if (error || (count && !items)) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_array *source = evio_calloc(1, sizeof(*source));
    error = evio_rx_buffer_init(&source->items, type, count, NULL);
    for (size_t i = 0; !error && i < count; ++i) {
        error = evio_rx_buffer_push(&source->items, (const char *)items + (i * type->size));
    }
    if (error) {
        evio_rx_buffer_fini(&source->items, NULL);
        evio_free(source);
        errno = error;
        return NULL;
    }
    evio_rx_observable_init(&source->base, &array_ops, type, NULL, (evio_rx_capture) {0});
    return &source->base;
}
