#include <errno.h>

#include "evio_rx_node.h"
#include "evio_rx_buffer.h"
#include "evio_alloc.h"

typedef struct {
    evio_rx_observable base;
    evio_rx_observable **sources;
    size_t count;
    evio_rx_combine_cb combine;
    size_t capacity;
} evio_rx_fan_in;

typedef struct {
    evio_rx_node base;
    evio_rx_node **inputs;
    size_t remaining;
} evio_rx_fan_in_node;

typedef struct {
    evio_rx_fan_in_node base;
    evio_rx_buffer *latest;
    const void **values;
    evio_rx_buffer out;
    size_t ready;
    bool receiving;
} evio_rx_combine_node;

typedef struct {
    evio_rx_combine_node base;
    evio_rx_ring *queues;
    size_t empty;               /**< The number of empty queues. */
} evio_rx_zip_node;

static void fan_in_fini(evio_rx_node *base)
{
    evio_rx_fan_in_node *node = (evio_rx_fan_in_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    evio_rx_free(base->root, node->inputs, source->count * sizeof(*node->inputs));
}

static int fan_in_init(evio_rx_node *base)
{
    evio_rx_fan_in_node *node = (evio_rx_fan_in_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    node->remaining = source->count;
    node->inputs = evio_rx_alloc(base->root, source->count * sizeof(*node->inputs));
    for (size_t i = 0; i < source->count; ++i) {
        node->inputs[i] = evio_rx_node_new(base, source->sources[i], i);
        if (!node->inputs[i]) {
            int error = errno;
            fan_in_fini(base);
            return error;
        }
    }
    return 0;
}

static void fan_in_reset(evio_rx_node *base)
{
    evio_rx_fan_in_node *node = (evio_rx_fan_in_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    node->remaining = source->count;
}

static void fan_in_start(evio_rx_node *base)
{
    evio_rx_fan_in_node *node = (evio_rx_fan_in_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    for (size_t i = 0; i < source->count && !base->closed; ++i) {
        evio_rx_node_start(node->inputs[i]);
    }
}

static void combine_fini(evio_rx_node *base)
{
    evio_rx_combine_node *node = (evio_rx_combine_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    evio_rx_buffer_fini(&node->out, base->root);
    evio_rx_free(base->root, node->values, source->count * sizeof(*node->values));
    fan_in_fini(base);
}

static int combine_init(evio_rx_node *base)
{
    int error = fan_in_init(base);
    if (error) {
        return error;
    }
    evio_rx_combine_node *node = (evio_rx_combine_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    node->values = evio_rx_alloc(base->root, source->count * sizeof(*node->values));
    error = evio_rx_buffer_init(&node->out, source->base.type, 1, base->root);
    if (error) {
        combine_fini(base);
    }
    return error;
}

static void combine_emit(evio_rx_node *base)
{
    evio_rx_combine_node *node = (evio_rx_combine_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    int error = source->combine(source->base.capture.data, node->out.data,
                                node->values, source->count);
    if (error) {
        evio_rx_fail(base->root, error);
        return;
    }
    node->out.count = 1;
    evio_rx_node_next(base, node->out.data);
    evio_rx_buffer_clear(&node->out);
}

static void latest_clear(evio_rx_node *base)
{
    evio_rx_combine_node *node = (evio_rx_combine_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    for (size_t i = 0; i < source->count; ++i) {
        evio_rx_buffer_clear(&node->latest[i]);
    }
}

static void latest_stop(evio_rx_node *base)
{
    evio_rx_combine_node *node = (evio_rx_combine_node *)base;
    if (!node->receiving) {
        latest_clear(base);
    }
}

static void latest_reset(evio_rx_node *base)
{
    evio_rx_combine_node *node = (evio_rx_combine_node *)base;
    latest_clear(base);
    node->ready = 0;
    fan_in_reset(base);
}

static void latest_fini(evio_rx_node *base)
{
    evio_rx_combine_node *node = (evio_rx_combine_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    for (size_t i = 0; i < source->count; ++i) {
        evio_rx_buffer_fini(&node->latest[i], base->root);
    }
    evio_rx_free(base->root, node->latest, source->count * sizeof(*node->latest));
    combine_fini(base);
}

static int latest_init(evio_rx_node *base)
{
    int error = combine_init(base);
    if (error) {
        return error;
    }
    evio_rx_combine_node *node = (evio_rx_combine_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    node->latest = evio_rx_alloc(base->root, source->count * sizeof(*node->latest));
    for (size_t i = 0; i < source->count; ++i) {
        error = evio_rx_buffer_init(&node->latest[i], source->sources[i]->type, 1, base->root);
        if (error) {
            latest_fini(base);
            return error;
        }
        node->values[i] = node->latest[i].data;
    }
    return 0;
}

static void latest_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_combine_node *node = (evio_rx_combine_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    evio_rx_buffer *latest = &node->latest[input->index];
    node->receiving = true;
    bool first = !latest->count;
    evio_rx_buffer_clear(latest);
    if (!base->closed) {
        int error = evio_rx_buffer_push(latest, value);
        if (error) {
            evio_rx_fail(base->root, error);
        } else if (!base->closed) {
            if (first) {
                node->ready++;
            }
            if (node->ready == source->count) {
                combine_emit(base);
            }
        }
    }
    node->receiving = false;
    if (base->closed) {
        latest_clear(base);
    }
}

static void latest_end(evio_rx_node *base, evio_rx_node *input, int error)
{
    evio_rx_combine_node *node = (evio_rx_combine_node *)base;
    if (error || !node->latest[input->index].count || !--node->base.remaining) {
        evio_rx_node_finish(base, error);
    }
}

static void merge_end(evio_rx_node *base, evio_rx_node *input, int error)
{
    evio_rx_fan_in_node *node = (evio_rx_fan_in_node *)base;
    if (error || !--node->remaining) {
        evio_rx_node_finish(base, error);
    }
}

static void fan_in_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_node_next(base, value);
}

static void fan_in_next_batch(evio_rx_node *base, evio_rx_node *input, const void *values,
                              size_t count)
{
    evio_rx_node_next_batch(base, values, count);
}

static void fan_in_free(evio_rx_observable *base)
{
    evio_rx_fan_in *source = (evio_rx_fan_in *)base;
    for (size_t i = 0; i < source->count; ++i) {
        evio_rx_observable_free(source->sources[i]);
    }
    evio_free(source->sources);
    evio_free(source);
}

static const evio_rx_ops merge_ops = {
    .node_size = sizeof(evio_rx_fan_in_node),
    .init = fan_in_init,
    .start = fan_in_start,
    .reset = fan_in_reset,
    .fini = fan_in_fini,
    .next = fan_in_next,
    .next_batch = fan_in_next_batch,
    .end = merge_end,
    .free = fan_in_free,
};

static evio_rx_observable *fan_in_new(evio_rx_observable *const *sources, size_t count,
                                      const evio_rx_ops *ops, const evio_rx_type *type)
{
    if (!sources || !count) {
        errno = EINVAL;
        return NULL;
    }
    if (count > SIZE_MAX / sizeof(*sources)) {
        errno = EOVERFLOW;
        return NULL;
    }
    evio_loop *loop = NULL;
    for (size_t i = 0; i < count; ++i) {
        evio_rx_observable *input = sources[i];
        if (!input || (!type && i && sources[0]->type != input->type) ||
            (loop && input->loop && loop != input->loop)) {
            errno = EINVAL;
            return NULL;
        }
        if (input->loop) {
            loop = input->loop;
        }
    }
    evio_rx_fan_in *source = evio_calloc(1, sizeof(*source));
    evio_rx_observable_init(&source->base, ops, type ? type : sources[0]->type,
                            loop, (evio_rx_capture) {0});
    source->sources = evio_malloc(count * sizeof(*source->sources));
    source->count = count;
    for (size_t i = 0; i < count; ++i) {
        source->sources[i] = evio_rx_observable_ref(sources[i]);
    }
    return &source->base;
}

evio_rx_observable *evio_rx_merge(evio_rx_observable *const *sources, size_t count)
{
    return fan_in_new(sources, count, &merge_ops, NULL);
}

// Input 0 is the notifier, input 1 the source.
static void take_until_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    if (!input->index) {
        evio_rx_node_finish(base, 0);
        return;
    }
    evio_rx_node_next(base, value);
}

static void take_until_next_batch(evio_rx_node *base, evio_rx_node *input, const void *values,
                                  size_t count)
{
    if (!input->index) {
        evio_rx_node_finish(base, 0);
        return;
    }
    evio_rx_node_next_batch(base, values, count);
}

static void take_until_end(evio_rx_node *base, evio_rx_node *input, int error)
{
    if (error || input->index) {
        evio_rx_node_finish(base, error);
    }
}

static const evio_rx_ops take_until_ops = {
    .node_size = sizeof(evio_rx_fan_in_node),
    .init = fan_in_init,
    .start = fan_in_start,
    .fini = fan_in_fini,
    .next = take_until_next,
    .next_batch = take_until_next_batch,
    .end = take_until_end,
    .free = fan_in_free,
};

evio_rx_observable *evio_rx_take_until(evio_rx_observable *source, evio_rx_observable *notifier)
{
    if (!source || !notifier) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_observable *sources[] = {notifier, source};
    return fan_in_new(sources, 2, &take_until_ops, source->type);
}

static const evio_rx_ops latest_ops = {
    .node_size = sizeof(evio_rx_combine_node),
    .init = latest_init,
    .start = fan_in_start,
    .stop = latest_stop,
    .reset = latest_reset,
    .fini = latest_fini,
    .next = latest_next,
    .end = latest_end,
    .free = fan_in_free,
};

static evio_rx_observable *combine_new(evio_rx_observable *const *sources, size_t count,
                                       evio_rx_combiner combiner, const evio_rx_ops *ops)
{
    if (!combiner.cb || evio_rx_type_check(combiner.type)) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_fan_in *source = (evio_rx_fan_in *)fan_in_new(sources, count, ops, combiner.type);
    if (!source) {
        return NULL;
    }
    source->combine = combiner.cb;
    source->base.capture = combiner.capture;
    return &source->base;
}

evio_rx_observable *evio_rx_combine_latest(evio_rx_observable *const *sources, size_t count,
                                           evio_rx_combiner combiner)
{
    if (count > SIZE_MAX / sizeof(evio_rx_buffer)) {
        errno = EOVERFLOW;
        return NULL;
    }
    return combine_new(sources, count, combiner, &latest_ops);
}

static void zip_clear(evio_rx_node *base)
{
    evio_rx_zip_node *node = (evio_rx_zip_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    for (size_t i = 0; i < source->count; ++i) {
        evio_rx_ring_clear(&node->queues[i]);
    }
    node->empty = source->count;
}

static void zip_stop(evio_rx_node *base)
{
    evio_rx_zip_node *node = (evio_rx_zip_node *)base;
    if (!node->base.receiving) {
        zip_clear(base);
    }
}

static void zip_reset(evio_rx_node *base)
{
    zip_clear(base);
    fan_in_reset(base);
}

static void zip_fini(evio_rx_node *base)
{
    evio_rx_zip_node *node = (evio_rx_zip_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    for (size_t i = 0; i < source->count; ++i) {
        evio_rx_ring_fini(&node->queues[i], base->root);
    }
    evio_rx_free(base->root, node->queues, source->count * sizeof(*node->queues));
    combine_fini(base);
}

static int zip_init(evio_rx_node *base)
{
    int error = combine_init(base);
    if (error) {
        return error;
    }
    evio_rx_zip_node *node = (evio_rx_zip_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    node->queues = evio_rx_alloc(base->root, source->count * sizeof(*node->queues));
    node->empty = source->count;
    for (size_t i = 0; i < source->count; ++i) {
        error = evio_rx_ring_init(&node->queues[i], source->sources[i]->type, source->capacity,
                                  base->root);
        if (error) {
            zip_fini(base);
            return error;
        }
    }
    return 0;
}

// Emits the tuple that `value` of the only empty input completes, without queueing
// the value; completes once a drained input has completed.
static void zip_emit(evio_rx_node *base, size_t index, const void *value)
{
    evio_rx_zip_node *node = (evio_rx_zip_node *)base;
    evio_rx_fan_in *source = (evio_rx_fan_in *)base->source;
    for (size_t i = 0; i < source->count; ++i) {
        node->base.values[i] = i == index ? value : evio_rx_ring_front(&node->queues[i]);
    }
    combine_emit(base);
    if (base->closed) {
        return;
    }
    bool completed = false;
    for (size_t i = 0; i < source->count; ++i) {
        evio_rx_ring *queue = &node->queues[i];
        if (i == index) {
            continue;
        }
        evio_rx_value_destroy(queue->type, evio_rx_ring_shift(queue));
        if (!queue->count) {
            node->empty++;
            completed |= node->base.base.inputs[i]->closed;
        }
    }
    if (completed) {
        evio_rx_node_finish(base, 0);
    }
}

static void zip_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_zip_node *node = (evio_rx_zip_node *)base;
    evio_rx_ring *queue = &node->queues[input->index];
    node->base.receiving = true;
    bool was_empty = !queue->count;
    if (was_empty && node->empty == 1) {
        zip_emit(base, input->index, value);
    } else {
        int error = evio_rx_ring_push(queue, value);
        if (error) {
            evio_rx_fail(base->root, error);
        } else {
            if (was_empty) {
                node->empty--;
            }
        }
    }
    node->base.receiving = false;
    if (base->closed) {
        zip_clear(base);
    }
}

static void zip_end(evio_rx_node *base, evio_rx_node *input, int error)
{
    if (error) {
        evio_rx_node_finish(base, error);
        return;
    }
    evio_rx_zip_node *node = (evio_rx_zip_node *)base;
    if (!node->queues[input->index].count) {
        evio_rx_node_finish(base, 0);
    }
}

static const evio_rx_ops zip_ops = {
    .node_size = sizeof(evio_rx_zip_node),
    .init = zip_init,
    .start = fan_in_start,
    .stop = zip_stop,
    .reset = zip_reset,
    .fini = zip_fini,
    .next = zip_next,
    .end = zip_end,
    .free = fan_in_free,
};

evio_rx_observable *evio_rx_zip(evio_rx_observable *const *sources, size_t count,
                                evio_rx_combiner combiner, size_t capacity)
{
    if (!capacity) {
        errno = EINVAL;
        return NULL;
    }
    if (count > SIZE_MAX / sizeof(evio_rx_ring)) {
        errno = EOVERFLOW;
        return NULL;
    }
    for (size_t i = 0; sources && i < count; ++i) {
        int error = sources[i] ? evio_rx_ring_check(sources[i]->type, capacity) : 0;
        if (error) {
            errno = error;
            return NULL;
        }
    }
    evio_rx_fan_in *source = (evio_rx_fan_in *)combine_new(sources, count, combiner, &zip_ops);
    if (!source) {
        return NULL;
    }
    source->capacity = capacity;
    return &source->base;
}
