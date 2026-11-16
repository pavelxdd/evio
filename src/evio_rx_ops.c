#include <errno.h>

#include "evio_rx_node.h"
#include "evio_rx_buffer.h"
#include "evio_alloc.h"

typedef struct {
    evio_rx_observable base;
    evio_rx_observable *source;
    union {
        evio_rx_map_cb map;
        evio_rx_filter_cb filter;
        evio_rx_map_batch_cb map_batch;
        evio_rx_filter_batch_cb filter_batch;
        evio_rx_scan_cb scan;
        evio_rx_equal_cb equal;
    } cb;
    evio_rx_buffer seed;
    size_t slots;               /**< Output values per callback of batch operators. */
} evio_rx_unary;

typedef struct {
    evio_rx_node base;
    evio_rx_node *input;
    evio_rx_buffer out;
    evio_rx_buffer state;
    alignas(max_align_t) unsigned char values[];    /**< Storage of `out` and `state`. */
} evio_rx_unary_node;

// The size of a node with `slots` values of `type`, or 0 on overflow.
static size_t unary_node_size(const evio_rx_type *type, size_t slots)
{
    size_t padding = type->alignment > alignof(max_align_t) ? type->alignment - 1 : 0;
    size_t limit = SIZE_MAX - sizeof(evio_rx_unary_node) - padding;
    if (slots && type->size > limit / slots) {
        return 0;
    }
    return sizeof(evio_rx_unary_node) + padding + (slots * type->size);
}

// Outputs of one batch callback fit in about 256 bytes.
static size_t batch_slots(const evio_rx_type *type)
{
    size_t slots = 256 / type->size;
    if (slots > EVIO_RX_BATCH) {
        return EVIO_RX_BATCH;
    }
    return slots ? slots : 1;
}

static unsigned char *unary_values(evio_rx_unary_node *node)
{
    size_t alignment = node->base.source->type->alignment;
    return node->values + (-(uintptr_t)node->values & (alignment - 1));
}

static int unary_init(evio_rx_node *base)
{
    evio_rx_unary_node *node = (evio_rx_unary_node *)base;
    evio_rx_unary *source = (evio_rx_unary *)base->source;
    node->input = evio_rx_node_new(base, source->source, 0);
    return node->input ? 0 : errno;
}

static void unary_start(evio_rx_node *base)
{
    evio_rx_unary_node *node = (evio_rx_unary_node *)base;
    evio_rx_node_start(node->input);
}

static int out_init(evio_rx_node *base)
{
    evio_rx_unary_node *node = (evio_rx_unary_node *)base;
    evio_rx_buffer_wrap(&node->out, base->source->type, unary_values(node));
    return unary_init(base);
}

static void state_clear(evio_rx_node *base)
{
    evio_rx_unary_node *node = (evio_rx_unary_node *)base;
    evio_rx_buffer_clear(&node->out);
    evio_rx_buffer_clear(&node->state);
}

static int seed_copy(evio_rx_unary_node *node)
{
    evio_rx_unary *source = (evio_rx_unary *)node->base.source;
    if (!source->seed.count) {
        return 0;
    }
    return evio_rx_buffer_push(&node->state, source->seed.data);
}

static int state_init(evio_rx_node *base)
{
    evio_rx_unary_node *node = (evio_rx_unary_node *)base;
    const evio_rx_type *type = base->source->type;
    unsigned char *values = unary_values(node);
    evio_rx_buffer_wrap(&node->out, type, values);
    evio_rx_buffer_wrap(&node->state, type, values + type->size);
    int error = seed_copy(node);
    if (!error) {
        error = unary_init(base);
    }
    if (error) {
        state_clear(base);
    }
    return error;
}

// A restarted run copies the seed again.
static void state_start(evio_rx_node *base)
{
    evio_rx_unary_node *node = (evio_rx_unary_node *)base;
    if (!node->state.count) {
        int error = seed_copy(node);
        if (error) {
            evio_rx_fail(base->root, error);
            return;
        }
    }
    evio_rx_node_start(node->input);
}

static void map_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_unary_node *node = (evio_rx_unary_node *)base;
    evio_rx_unary *source = (evio_rx_unary *)base->source;
    int error = source->cb.map(source->base.capture.data, node->out.data, value);
    if (error) {
        evio_rx_fail(base->root, error);
        return;
    }
    evio_rx_node_next(base, node->out.data);
    evio_rx_value_destroy(source->base.type, node->out.data);
}

static void filter_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_unary *source = (evio_rx_unary *)base->source;
    if (source->cb.filter(source->base.capture.data, value)) {
        evio_rx_node_next(base, value);
    }
}

static void map_batch_next_batch(evio_rx_node *base, evio_rx_node *input, const void *values,
                                 size_t count)
{
    evio_rx_unary_node *node = (evio_rx_unary_node *)base;
    evio_rx_unary *source = (evio_rx_unary *)base->source;
    size_t size = input->source->type->size;
    const char *value = values;
    while (count && !base->closed) {
        size_t batch = count < source->slots ? count : source->slots;
        int error = source->cb.map_batch(source->base.capture.data, node->out.data, value, batch);
        if (error) {
            evio_rx_fail(base->root, error);
            return;
        }
        node->out.count = batch;
        evio_rx_node_next_batch(base, node->out.data, batch);
        evio_rx_buffer_clear(&node->out);
        value += batch * size;
        count -= batch;
    }
}

static void map_batch_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    map_batch_next_batch(base, input, value, 1);
}

static void filter_batch_next_batch(evio_rx_node *base, evio_rx_node *input, const void *values,
                                    size_t count)
{
    evio_rx_unary_node *node = (evio_rx_unary_node *)base;
    evio_rx_unary *source = (evio_rx_unary *)base->source;
    size_t size = input->source->type->size;
    const char *value = values;
    while (count && !base->closed) {
        size_t batch = count < source->slots ? count : source->slots;
        size_t kept = 0;
        int error = source->cb.filter_batch(source->base.capture.data, node->out.data, &kept,
                                            value, batch);
        if (error) {
            evio_rx_fail(base->root, error);
            return;
        }
        if (kept) {
            node->out.count = kept;
            evio_rx_node_next_batch(base, node->out.data, kept);
            evio_rx_buffer_clear(&node->out);
        }
        value += batch * size;
        count -= batch;
    }
}

static void filter_batch_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    filter_batch_next_batch(base, input, value, 1);
}

static void state_swap(evio_rx_unary_node *node)
{
    evio_rx_buffer_clear(&node->state);
    evio_rx_buffer previous = node->state;
    node->state = node->out;
    node->out = previous;
}

static void scan_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_unary_node *node = (evio_rx_unary_node *)base;
    evio_rx_unary *source = (evio_rx_unary *)base->source;
    int error = source->cb.scan(source->base.capture.data, node->out.data, node->state.data, value);
    if (error) {
        evio_rx_fail(base->root, error);
        return;
    }
    node->out.count = 1;
    state_swap(node);
    evio_rx_node_next(base, node->state.data);
}

static void distinct_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_unary_node *node = (evio_rx_unary_node *)base;
    evio_rx_unary *source = (evio_rx_unary *)base->source;
    if (node->state.count) {
        bool equal = source->cb.equal(source->base.capture.data, node->state.data, value);
        if (equal || base->closed) {
            return;
        }
    }
    int error = evio_rx_buffer_push(&node->out, value);
    if (error) {
        evio_rx_fail(base->root, error);
        return;
    }
    state_swap(node);
    evio_rx_node_next(base, node->state.data);
}

static void unary_free(evio_rx_observable *base)
{
    evio_rx_unary *source = (evio_rx_unary *)base;
    evio_rx_observable_free(source->source);
    evio_rx_buffer_fini(&source->seed, NULL);
    evio_free(source);
}

static const evio_rx_ops map_ops = {
    .node_size = sizeof(evio_rx_unary_node),
    .init = out_init,
    .start = unary_start,
    .next = map_next,
    .end = evio_rx_forward_end,
    .free = unary_free,
};

static const evio_rx_ops filter_ops = {
    .node_size = sizeof(evio_rx_unary_node),
    .init = unary_init,
    .start = unary_start,
    .next = filter_next,
    .end = evio_rx_forward_end,
    .free = unary_free,
};

static const evio_rx_ops map_batch_ops = {
    .node_size = sizeof(evio_rx_unary_node),
    .init = out_init,
    .start = unary_start,
    .next = map_batch_next,
    .next_batch = map_batch_next_batch,
    .end = evio_rx_forward_end,
    .free = unary_free,
};

static const evio_rx_ops filter_batch_ops = {
    .node_size = sizeof(evio_rx_unary_node),
    .init = out_init,
    .start = unary_start,
    .next = filter_batch_next,
    .next_batch = filter_batch_next_batch,
    .end = evio_rx_forward_end,
    .free = unary_free,
};

static const evio_rx_ops scan_ops = {
    .node_size = sizeof(evio_rx_unary_node),
    .init = state_init,
    .start = state_start,
    .reset = state_clear,
    .fini = state_clear,
    .next = scan_next,
    .end = evio_rx_forward_end,
    .free = unary_free,
};

static const evio_rx_ops distinct_ops = {
    .node_size = sizeof(evio_rx_unary_node),
    .init = state_init,
    .start = unary_start,
    .reset = state_clear,
    .fini = state_clear,
    .next = distinct_next,
    .end = evio_rx_forward_end,
    .free = unary_free,
};

static evio_rx_unary *unary_new(evio_rx_observable *source, const evio_rx_ops *ops,
                                const evio_rx_type *type, size_t node_size, evio_rx_capture capture)
{
    evio_rx_unary *result = evio_calloc(1, sizeof(*result));
    evio_rx_observable_init(&result->base, ops, type, source->loop, capture);
    result->base.node_size = node_size;
    result->source = evio_rx_observable_ref(source);
    return result;
}

// Creates an operator with `slots` output values in its node.
static evio_rx_unary *out_new(evio_rx_observable *source, const evio_rx_ops *ops,
                              const evio_rx_type *type, size_t slots, evio_rx_capture capture)
{
    size_t node_size = unary_node_size(type, slots);
    if (!node_size) {
        errno = EOVERFLOW;
        return NULL;
    }
    evio_rx_unary *result = unary_new(source, ops, type, node_size, capture);
    result->slots = slots;
    return result;
}

evio_rx_observable *evio_rx_distinct_until_changed(evio_rx_observable *source,
                                                   evio_rx_equal_cb cb, evio_rx_capture capture)
{
    if (!source || !cb) {
        errno = EINVAL;
        return NULL;
    }
    size_t node_size = unary_node_size(source->type, 2);
    if (!node_size) {
        errno = EOVERFLOW;
        return NULL;
    }
    evio_rx_unary *result = unary_new(source, &distinct_ops, source->type, node_size, capture);
    result->cb.equal = cb;
    return &result->base;
}

evio_rx_observable *evio_rx_scan(evio_rx_observable *source, const evio_rx_type *type,
                                 const void *seed, evio_rx_scan_cb cb, evio_rx_capture capture)
{
    if (!source || !seed || !cb || evio_rx_type_check(type)) {
        errno = EINVAL;
        return NULL;
    }
    size_t node_size = unary_node_size(type, 2);
    if (!node_size) {
        errno = EOVERFLOW;
        return NULL;
    }
    evio_rx_buffer copy;
    int error = evio_rx_buffer_init(&copy, type, 1, NULL);
    if (!error) {
        error = evio_rx_buffer_push(&copy, seed);
    }
    if (error) {
        evio_rx_buffer_fini(&copy, NULL);
        errno = error;
        return NULL;
    }
    evio_rx_unary *result = unary_new(source, &scan_ops, type, node_size, capture);
    result->seed = copy;
    result->cb.scan = cb;
    return &result->base;
}

evio_rx_observable *evio_rx_map(evio_rx_observable *source, const evio_rx_type *type,
                                evio_rx_map_cb cb, evio_rx_capture capture)
{
    if (!source || !cb || evio_rx_type_check(type)) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_unary *result = out_new(source, &map_ops, type, 1, capture);
    if (!result) {
        return NULL;
    }
    result->cb.map = cb;
    return &result->base;
}

evio_rx_observable *evio_rx_filter(evio_rx_observable *source,
                                   evio_rx_filter_cb cb, evio_rx_capture capture)
{
    if (!source || !cb) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_unary *result = unary_new(source, &filter_ops, source->type, sizeof(evio_rx_unary_node),
                                      capture);
    result->cb.filter = cb;
    return &result->base;
}

evio_rx_observable *evio_rx_map_batch(evio_rx_observable *source, const evio_rx_type *type,
                                      evio_rx_map_batch_cb cb, evio_rx_capture capture)
{
    if (!source || !cb || evio_rx_type_check(type)) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_unary *result = out_new(source, &map_batch_ops, type, batch_slots(type), capture);
    if (!result) {
        return NULL;
    }
    result->cb.map_batch = cb;
    return &result->base;
}

evio_rx_observable *evio_rx_filter_batch(evio_rx_observable *source,
                                         evio_rx_filter_batch_cb cb, evio_rx_capture capture)
{
    if (!source || !cb) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_unary *result = out_new(source, &filter_batch_ops, source->type,
                                    batch_slots(source->type), capture);
    if (!result) {
        return NULL;
    }
    result->cb.filter_batch = cb;
    return &result->base;
}
