#include <errno.h>
#include <string.h>

#include "evio_rx_node.h"
#include "evio_rx_buffer.h"
#include "evio_alloc.h"

typedef struct {
    evio_rx_observable base;
    evio_rx_observable *source;
    evio_rx_project_cb project;
    size_t capacity;
    size_t concurrency;
} evio_rx_flatten;

typedef struct {
    evio_rx_node base;
    evio_rx_node *outer;
    evio_rx_node *inner;
    bool outer_done;
} evio_rx_switch_node;

// Returns the projected description, or NULL once the node is closed.
static evio_rx_observable *project(evio_rx_node *base, const void *value)
{
    evio_rx_flatten *source = (evio_rx_flatten *)base->source;
    errno = 0;
    evio_rx_observable *inner = source->project(source->base.capture.data, value);
    if (!inner) {
        if (!base->closed) {
            evio_rx_fail(base->root, errno);
        }
        return NULL;
    }
    if (base->closed) {
        evio_rx_observable_free(inner);
        return NULL;
    }
    if (inner->type != source->base.type) {
        evio_rx_observable_free(inner);
        evio_rx_fail(base->root, EINVAL);
        return NULL;
    }
    return inner;
}

// Input 0 is the outer node; inner nodes have positive indexes.
static evio_rx_node *project_inner(evio_rx_node *base, const void *value, size_t index)
{
    evio_rx_observable *inner = project(base, value);
    if (!inner) {
        return NULL;
    }
    evio_rx_node *child = evio_rx_node_adopt(base, inner, index);
    if (!child) {
        evio_rx_fail(base->root, errno);
    }
    return base->closed ? NULL : child;
}

static int switch_init(evio_rx_node *base)
{
    evio_rx_switch_node *node = (evio_rx_switch_node *)base;
    evio_rx_flatten *source = (evio_rx_flatten *)base->source;
    node->outer = evio_rx_node_new(base, source->source, 0);
    return node->outer ? 0 : errno;
}

static void switch_start(evio_rx_node *base)
{
    evio_rx_switch_node *node = (evio_rx_switch_node *)base;
    evio_rx_node_start(node->outer);
}

// Inner nodes are freed with the run state.
static void switch_reset(evio_rx_node *base)
{
    evio_rx_switch_node *node = (evio_rx_switch_node *)base;
    node->inner = NULL;
    node->outer_done = false;
}

static void switch_retire_inner(evio_rx_switch_node *node)
{
    evio_rx_node *inner = node->inner;
    node->inner = NULL;
    if (inner) {
        evio_rx_node_retire(inner);
    }
}

// Takes the reference `source`; a replaced inner of the same description is reused.
static evio_rx_node *switch_inner(evio_rx_switch_node *node, evio_rx_node *old,
                                  evio_rx_observable *source)
{
    if (old && old->source == source && evio_rx_node_rewind(old)) {
        evio_rx_observable_free(source);
        return old;
    }
    if (old) {
        evio_rx_node_retire(old);
    }
    evio_rx_node *child = evio_rx_node_adopt(&node->base, source, 1);
    if (!child) {
        evio_rx_fail(node->base.root, errno);
    }
    return child;
}

static void switch_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_switch_node *node = (evio_rx_switch_node *)base;
    if (input->index) {
        evio_rx_node_next(base, value);
        return;
    }
    evio_rx_node *old = node->inner;
    node->inner = NULL;
    if (old) {
        evio_rx_node_cancel(old);
    }
    evio_rx_observable *source = project(base, value);
    if (!source) {
        if (old) {
            evio_rx_node_retire(old);
        }
        return;
    }
    evio_rx_node *inner = switch_inner(node, old, source);
    if (!inner || base->closed) {
        return;
    }
    node->inner = inner;
    evio_rx_node_start(inner);
}

static void switch_end(evio_rx_node *base, evio_rx_node *input, int error)
{
    evio_rx_switch_node *node = (evio_rx_switch_node *)base;
    if (error) {
        evio_rx_node_finish(base, error);
        return;
    }
    if (input->index) {
        switch_retire_inner(node);
    } else {
        node->outer_done = true;
    }
    if (node->outer_done && !node->inner) {
        evio_rx_node_finish(base, 0);
    }
}

typedef struct {
    evio_rx_node base;
    evio_rx_node *outer;
    evio_rx_node **slots;
    evio_rx_ring queue;
    size_t active;
    bool outer_done;
    bool pumping;
    bool copying;
} evio_rx_flatten_node;

static void flatten_fini(evio_rx_node *base)
{
    evio_rx_flatten_node *node = (evio_rx_flatten_node *)base;
    evio_rx_flatten *source = (evio_rx_flatten *)base->source;
    evio_rx_ring_fini(&node->queue, base->root);
    evio_rx_free(base->root, node->slots, source->concurrency * sizeof(*node->slots));
}

static int flatten_init(evio_rx_node *base)
{
    evio_rx_flatten_node *node = (evio_rx_flatten_node *)base;
    evio_rx_flatten *source = (evio_rx_flatten *)base->source;
    int error = evio_rx_ring_init(&node->queue, source->source->type, source->capacity, base->root);
    if (error) {
        return error;
    }
    node->slots = evio_rx_alloc(base->root, source->concurrency * sizeof(*node->slots));
    node->outer = evio_rx_node_new(base, source->source, 0);
    if (!node->outer) {
        error = errno;
        flatten_fini(base);
        return error;
    }
    return 0;
}

static void flatten_start(evio_rx_node *base)
{
    evio_rx_flatten_node *node = (evio_rx_flatten_node *)base;
    evio_rx_node_start(node->outer);
}

static void flatten_stop(evio_rx_node *base)
{
    evio_rx_flatten_node *node = (evio_rx_flatten_node *)base;
    // A copy can cancel before its queue slot contains a live value.
    if (!node->copying) {
        evio_rx_ring_clear(&node->queue);
    }
}

static void flatten_reset(evio_rx_node *base)
{
    evio_rx_flatten_node *node = (evio_rx_flatten_node *)base;
    evio_rx_flatten *source = (evio_rx_flatten *)base->source;
    evio_rx_ring_clear(&node->queue);
    memset(node->slots, 0, source->concurrency * sizeof(*node->slots));
    node->active = 0;
    node->outer_done = false;
}

static void flatten_launch(evio_rx_flatten_node *node, const void *value)
{
    evio_rx_flatten *source = (evio_rx_flatten *)node->base.source;
    for (size_t i = 0; i < source->concurrency; ++i) {
        if (node->slots[i]) {
            continue;
        }
        evio_rx_node *inner = project_inner(&node->base, value, i + 1);
        if (!inner) {
            return;
        }
        node->slots[i] = inner;
        node->active++;
        evio_rx_node_start(inner);
        return;
    }
}

static void flatten_pump(evio_rx_flatten_node *node, const void *value)
{
    evio_rx_node *base = &node->base;
    evio_rx_flatten *source = (evio_rx_flatten *)base->source;
    if (node->pumping || base->closed) {
        return;
    }
    node->pumping = true;
    if (value) {
        flatten_launch(node, value);
    }
    while (!base->closed && node->active < source->concurrency && node->queue.count) {
        void *waiting = evio_rx_ring_shift(&node->queue);
        flatten_launch(node, waiting);
        evio_rx_value_destroy(node->queue.type, waiting);
    }
    node->pumping = false;
    if (!base->closed && node->outer_done && !node->active) {
        evio_rx_node_finish(base, 0);
    }
}

static void flatten_next(evio_rx_node *base, evio_rx_node *input, const void *value)
{
    evio_rx_flatten_node *node = (evio_rx_flatten_node *)base;
    evio_rx_flatten *source = (evio_rx_flatten *)base->source;
    if (input->index) {
        evio_rx_node_next(base, value);
        return;
    }
    if (node->active < source->concurrency && !node->pumping && !node->queue.count) {
        flatten_pump(node, value);
        return;
    }
    node->copying = true;
    int error = evio_rx_ring_push(&node->queue, value);
    node->copying = false;
    if (error) {
        evio_rx_fail(base->root, error);
    }
    if (base->closed) {
        evio_rx_ring_clear(&node->queue);
        return;
    }
    flatten_pump(node, NULL);
}

static void flatten_end(evio_rx_node *base, evio_rx_node *input, int error)
{
    evio_rx_flatten_node *node = (evio_rx_flatten_node *)base;
    if (error) {
        evio_rx_node_finish(base, error);
        return;
    }
    if (input->index) {
        node->slots[input->index - 1] = NULL;
        node->active--;
        evio_rx_node_retire(input);
    } else {
        node->outer_done = true;
    }
    flatten_pump(node, NULL);
}

static void flatten_free(evio_rx_observable *base)
{
    evio_rx_flatten *source = (evio_rx_flatten *)base;
    evio_rx_observable_free(source->source);
    evio_free(source);
}

static const evio_rx_ops switch_ops = {
    .node_size = sizeof(evio_rx_switch_node),
    .init = switch_init,
    .start = switch_start,
    .reset = switch_reset,
    .next = switch_next,
    .end = switch_end,
    .free = flatten_free,
};

static const evio_rx_ops flatten_ops = {
    .node_size = sizeof(evio_rx_flatten_node),
    .init = flatten_init,
    .start = flatten_start,
    .stop = flatten_stop,
    .reset = flatten_reset,
    .fini = flatten_fini,
    .next = flatten_next,
    .end = flatten_end,
    .free = flatten_free,
};

static evio_rx_observable *flatten_new(evio_rx_observable *source, evio_rx_project project,
                                       const evio_rx_ops *ops, size_t concurrency, size_t capacity)
{
    if (!source || evio_rx_type_check(project.type) || !project.cb) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_flatten *result = evio_calloc(1, sizeof(*result));
    evio_rx_observable_init(&result->base, ops, project.type, source->loop, project.capture);
    result->source = evio_rx_observable_ref(source);
    result->project = project.cb;
    result->capacity = capacity;
    result->concurrency = concurrency;
    return &result->base;
}

static evio_rx_observable *bounded_new(evio_rx_observable *source, evio_rx_project project,
                                       size_t concurrency, size_t capacity)
{
    if (!concurrency || !capacity) {
        errno = EINVAL;
        return NULL;
    }
    if (concurrency > SIZE_MAX / sizeof(evio_rx_node *)) {
        errno = EOVERFLOW;
        return NULL;
    }
    int error = source ? evio_rx_ring_check(source->type, capacity) : 0;
    if (error) {
        errno = error;
        return NULL;
    }
    return flatten_new(source, project, &flatten_ops, concurrency, capacity);
}

evio_rx_observable *evio_rx_switch_map(evio_rx_observable *source, evio_rx_project project)
{
    return flatten_new(source, project, &switch_ops, 0, 0);
}

evio_rx_observable *evio_rx_concat_map(evio_rx_observable *source, evio_rx_project project,
                                       size_t capacity)
{
    return bounded_new(source, project, 1, capacity);
}

evio_rx_observable *evio_rx_merge_map(evio_rx_observable *source, evio_rx_project project,
                                      size_t concurrency, size_t capacity)
{
    return bounded_new(source, project, concurrency, capacity);
}
