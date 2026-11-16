#include <errno.h>
#include <string.h>

#include "evio_rx_node.h"
#include "evio_rx_buffer.h"
#include "evio_alloc.h"

typedef enum {
    EVIO_RX_NEXT,
    EVIO_RX_END,
    EVIO_RX_TICK,
} evio_rx_input;

struct evio_rx_message {
    evio_rx_node *node;
    evio_rx_timer_state *timer;
    uint64_t generation;
    evio_rx_buffer value;
    evio_rx_input input;
    int error;
};

#define RUN_ALIGN alignof(max_align_t)

static bool in_arena(const evio_rx_subscription *root, const void *ptr)
{
    return (uintptr_t)ptr - (uintptr_t)root->arena < root->arena_size;
}

void *evio_rx_alloc(evio_rx_subscription *root, size_t size)
{
    size_t step = size + (-size & (RUN_ALIGN - 1));
    if (step < size) {
        root->heap_blocks++;
        return evio_calloc(1, size);
    }
    if (!root->started) {
        root->run_size += step;
    }
    size_t list = (step / RUN_ALIGN) - 1;
    if (list < EVIO_RX_FREE_LISTS && root->free_lists[list]) {
        void *ptr = root->free_lists[list];
        root->free_lists[list] = *(void **)ptr;
        return memset(ptr, 0, size);
    }
    // The arena is zeroed when the run is created.
    if (step <= root->arena_size - root->arena_used) {
        void *ptr = root->arena + root->arena_used;
        root->arena_used += step;
        return ptr;
    }
    root->heap_blocks++;
    return evio_calloc(1, step);
}

void evio_rx_free(evio_rx_subscription *root, void *ptr, size_t size)
{
    if (!ptr) {
        return;
    }
    size_t list = ((size + (-size & (RUN_ALIGN - 1))) / RUN_ALIGN) - 1;
    if (list < EVIO_RX_FREE_LISTS) {
        *(void **)ptr = root->free_lists[list];
        root->free_lists[list] = ptr;
        return;
    }
    if (!in_arena(root, ptr)) {
        evio_free(ptr);
    }
}

void evio_rx_observable_init(evio_rx_observable *source, const evio_rx_ops *ops,
                             const evio_rx_type *type, evio_loop *loop, evio_rx_capture capture)
{
    *source = (evio_rx_observable) {
        .ops = ops,
        .type = type,
        .loop = loop,
        .capture = capture,
        .refs = 1,
        .node_size = ops->node_size,
    };
}

static evio_rx_node *first_leaf(evio_rx_node *node)
{
    while (node->child) {
        node = node->child;
    }
    return node;
}

// Children-first successor within the subtree of top.
static evio_rx_node *next_leaf(evio_rx_node *node, evio_rx_node *top)
{
    if (node == top) {
        return NULL;
    }
    if (node->next) {
        return first_leaf(node->next);
    }
    return node->parent;
}

static evio_rx_node *node_create(evio_rx_node *parent, evio_rx_observable *source, size_t index,
                                 bool owns_source)
{
    evio_rx_subscription *root = parent->root;
    if (source->loop && source->loop != root->loop) {
        if (owns_source) {
            evio_rx_observable_free(source);
        }
        errno = EINVAL;
        return NULL;
    }

    evio_rx_node *node = evio_rx_alloc(root, source->node_size);
    *node = (evio_rx_node) {
        .ops = source->ops,
        .source = source,
        .root = root,
        .parent = parent,
        .next = parent->child,
        .index = index,
        .owns_source = owns_source,
        .input_closed = true,
        .closed = parent->closed,
    };
    if (parent->child) {
        parent->child->prev = node;
    }
    parent->child = node;

    if (node->ops->init) {
        int error = node->ops->init(node);
        if (error) {
            errno = error > 0 ? error : EINVAL;
            return NULL;
        }
    }
    node->ready = true;
    return node;
}

evio_rx_node *evio_rx_node_new(evio_rx_node *parent, evio_rx_observable *source, size_t index)
{
    return node_create(parent, source, index, false);
}

evio_rx_node *evio_rx_node_adopt(evio_rx_node *parent, evio_rx_observable *source, size_t index)
{
    return node_create(parent, source, index, true);
}

void evio_rx_node_start(evio_rx_node *node)
{
    evio_rx_subscription *root = node->root;
    if (node->started || node->closed) {
        return;
    }
    node->started = true;
    node->input_closed = false;
    evio_rx_node *producer = root->producer;
    size_t gate = root->gate;
    root->producer = node;
    root->gate = root->depth;
    node->active++;
    node->ops->start(node);
    node->active--;
    root->producer = producer;
    root->gate = gate;
}

static void node_stop(evio_rx_node *node)
{
    if (!node->started || node->stopped) {
        return;
    }
    node->stopped = true;
    if (node->ops->stop) {
        node->ops->stop(node);
    }
}

// A closed node has a closed subtree, a stopped or unstarted node a stopped one.
static bool needs_cancel(const evio_rx_node *node, bool stopping)
{
    return stopping ? node->started && !node->stopped : !node->closed;
}

static evio_rx_node *first_uncancelled(evio_rx_node *node, bool stopping)
{
    for (;;) {
        evio_rx_node *child = node->child;
        while (child && !needs_cancel(child, stopping)) {
            child = child->next;
        }
        if (!child) {
            return node;
        }
        node = child;
    }
}

static evio_rx_node *next_uncancelled(evio_rx_node *node, evio_rx_node *top, bool stopping)
{
    if (node == top) {
        return NULL;
    }
    evio_rx_node *sibling = node->next;
    while (sibling && !needs_cancel(sibling, stopping)) {
        sibling = sibling->next;
    }
    return sibling ? first_uncancelled(sibling, stopping) : node->parent;
}

void evio_rx_node_cancel(evio_rx_node *node)
{
    evio_rx_node *child = node->child;
    while (child && !needs_cancel(child, false) && !needs_cancel(child, true)) {
        child = child->next;
    }
    if (!child) {
        node->closed = true;
        node->input_closed = true;
        node_stop(node);
        return;
    }
    for (evio_rx_node *current = first_uncancelled(node, false); current;
         current = next_uncancelled(current, node, false)) {
        current->closed = true;
        current->input_closed = true;
    }
    for (evio_rx_node *current = first_uncancelled(node, true); current;
         current = next_uncancelled(current, node, true)) {
        node_stop(current);
    }
}

static void node_unlink(evio_rx_node *node)
{
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        node->parent->child = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
    node->prev = NULL;
    node->next = NULL;
}

void evio_rx_node_retire(evio_rx_node *node)
{
    evio_rx_subscription *root = node->root;
    evio_rx_node_cancel(node);
    node_unlink(node);
    node->next = root->garbage;
    root->garbage = node;
    root->pending = true;
}

void evio_rx_node_finish(evio_rx_node *node, int error)
{
    if (node->closed) {
        return;
    }
    evio_rx_node_cancel(node);
    evio_rx_node *parent = node->parent;
    if (!parent->closed) {
        parent->ops->end(parent, node, error);
    }
}

void evio_rx_forward_end(evio_rx_node *node, evio_rx_node *input, int error)
{
    evio_rx_node_finish(node, error);
}

void evio_rx_run_close(evio_rx_subscription *root)
{
    if (root->closed) {
        return;
    }
    root->closed = true;
    root->sink.closed = true;
    root->pending = true;
    evio_rx_node *entry = root->sink.child;
    if (entry && !entry->closed) {
        evio_rx_node_cancel(entry);
    }
}

void evio_rx_fail(evio_rx_subscription *root, int error)
{
    if (root->closed) {
        return;
    }
    root->terminal_error = error > 0 ? error : EINVAL;
    root->terminal_pending = true;
    evio_rx_run_close(root);
}

static void observer_skip(void *ctx, const void *value)
{
}

static void sink_end(evio_rx_node *node, evio_rx_node *input, int error)
{
    evio_rx_subscription *root = (evio_rx_subscription *)node;
    root->terminal_error = error;
    root->terminal_pending = true;
    evio_rx_run_close(root);
}

// Values reach the observer directly from `evio_rx_node_next()`.
static const evio_rx_ops sink_ops = {
    .end = sink_end,
};

static evio_rx_message *queue_push(evio_rx_subscription *root, evio_rx_message message)
{
    if (root->queue_count == root->queue_capacity) {
        evio_rx_fail(root, EOVERFLOW);
        return NULL;
    }
    if (!root->queue) {
        root->queue = evio_rx_alloc(root, root->queue_capacity * sizeof(*root->queue));
    }
    size_t index = root->queue_head + root->queue_count++;
    if (index >= root->queue_capacity) {
        index -= root->queue_capacity;
    }
    root->pending = true;
    evio_rx_message *slot = &root->queue[index];
    *slot = message;
    return slot;
}

static evio_rx_message queue_shift(evio_rx_subscription *root)
{
    evio_rx_message message = root->queue[root->queue_head];
    if (++root->queue_head == root->queue_capacity) {
        root->queue_head = 0;
    }
    root->queue_count--;
    return message;
}

static void deliver(evio_rx_message *message)
{
    evio_rx_node *node = message->node;
    switch (message->input) {
        case EVIO_RX_NEXT:
            evio_rx_node_next(node, message->value.data);
            break;
        case EVIO_RX_END:
            evio_rx_node_finish(node, message->error);
            break;
        case EVIO_RX_TICK:
            if (!node->closed && message->timer->armed &&
                message->timer->generation == message->generation) {
                node->ops->tick(node);
            }
            break;
    }
}

static bool is_direct(const evio_rx_subscription *root, const evio_rx_node *node)
{
    return root->depth == root->gate && (!root->producer || root->producer == node);
}

// Queued input goes first; a delivery or a closed run leaves nothing to drain.
static void input(evio_rx_node *node, evio_rx_message message, const void *value)
{
    evio_rx_subscription *root = node->root;
    if (!is_direct(root, node)) {
        evio_rx_message *slot = queue_push(root, message);
        if (!slot || message.input != EVIO_RX_NEXT) {
            return;
        }
        // The slot is reserved before the copy hook, which may queue more input.
        root->depth++;
        int error = evio_rx_buffer_init(&slot->value, node->source->type, 1, root);
        if (!error) {
            error = evio_rx_buffer_push(&slot->value, value);
        }
        root->depth--;
        if (error) {
            evio_rx_fail(root, error);
        }
        return;
    }
    if (!root->depth && root->queue_count) {
        evio_rx_settle(root);
    }
    message.value.data = (void *)value;
    root->depth++;
    deliver(&message);
    root->depth--;
}

bool evio_rx_emit_slow(evio_rx_node *node, const void *value)
{
    evio_rx_subscription *root = node->root;
    if (node->input_closed) {
        return false;
    }
    evio_rx_enter(root);
    node->active++;
    if (value) {
        input(node, (evio_rx_message) { .node = node, .input = EVIO_RX_NEXT }, value);
    } else {
        evio_rx_fail(root, EINVAL);
    }
    if (root->pending && !root->depth) {
        evio_rx_settle(root);
    }
    bool open = !node->input_closed;
    node->active--;
    evio_rx_leave(root);
    return open;
}

bool evio_rx_emit_settle(evio_rx_node *node)
{
    evio_rx_subscription *root = node->root;
    evio_rx_enter(root);
    node->active++;
    evio_rx_settle(root);
    bool open = !node->input_closed;
    node->active--;
    evio_rx_leave(root);
    return open;
}

bool evio_rx_emit_each(evio_rx_node *node, const void *values, size_t count)
{
    size_t size = node->source->type->size;
    const char *value = values;
    for (size_t i = 0; i < count; ++i, value += size) {
        if (!evio_rx_emit(node, value)) {
            return false;
        }
    }
    return !node->input_closed;
}

void evio_rx_emit_end(evio_rx_node *node, int error)
{
    evio_rx_subscription *root = node->root;
    if (node->input_closed) {
        return;
    }
    evio_rx_enter(root);
    node->input_closed = true;
    node_stop(node);
    if (root->queue_count || !is_direct(root, node)) {
        input(node, (evio_rx_message) { .node = node, .input = EVIO_RX_END, .error = error }, NULL);
    } else {
        root->depth++;
        evio_rx_node_finish(node, error);
        root->depth--;
    }
    evio_rx_leave(root);
}

static void timer_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_rx_timer_state *timer = base->data;
    evio_rx_node *node = timer->node;
    evio_rx_subscription *root = node->root;
    evio_rx_enter(root);
    // A fired watcher was armed and not restarted since.
    if (!root->queue_count && is_direct(root, node)) {
        root->depth++;
        if (!node->closed) {
            node->ops->tick(node);
        }
        root->depth--;
        evio_rx_leave(root);
        return;
    }
    evio_rx_message message = {
        .node = node,
        .timer = timer,
        .generation = timer->generation,
        .input = EVIO_RX_TICK,
    };
    input(node, message, NULL);
    evio_rx_leave(root);
}

static void notify_terminal(evio_rx_subscription *root)
{
    root->terminal_pending = false;
    if (root->terminal_error) {
        if (root->observer.error) {
            root->observer.error(root->observer.ctx, root->terminal_error);
        }
    } else if (root->observer.complete) {
        root->observer.complete(root->observer.ctx);
    }
}

// Returns the size of the node memory, which the caller releases.
static size_t node_fini(evio_rx_node *node)
{
    if (node->ready && node->ops->fini) {
        node->ops->fini(node);
    }
    size_t size = node->source->node_size;
    if (node->owns_source) {
        evio_rx_observable_free(node->source);
    }
    return size;
}

static void node_free(evio_rx_node *node)
{
    evio_rx_free(node->root, node, node_fini(node));
}

static void tree_free(evio_rx_node *top)
{
    for (evio_rx_node *node = first_leaf(top); node;) {
        evio_rx_node *next = next_leaf(node, top);
        node_free(node);
        node = next;
    }
}

static bool is_active(evio_rx_node *top)
{
    for (evio_rx_node *node = first_leaf(top); node; node = next_leaf(node, top)) {
        if (node->active) {
            return true;
        }
    }
    return false;
}

// Frees one retired subtree that no frame uses.
static bool garbage_free(evio_rx_subscription *root)
{
    for (evio_rx_node **link = &root->garbage; *link; link = &(*link)->next) {
        evio_rx_node *tree = *link;
        if (!is_active(tree)) {
            *link = tree->next;
            tree_free(tree);
            return true;
        }
    }
    return false;
}

static void node_rewind(evio_rx_node *node)
{
    if (node->ops->reset) {
        node->ops->reset(node);
    }
    // Inputs stay closed until the next start.
    node->started = false;
    node->stopped = false;
    node->closed = false;
}

// Frees the subtrees created during the run and rewinds the others, children first.
static void tree_reset(evio_rx_subscription *root)
{
    evio_rx_node *top = root->sink.child;
    for (evio_rx_node *node = first_leaf(top); node;) {
        evio_rx_node *next = next_leaf(node, top);
        if (node->owns_source) {
            node_unlink(node);
            tree_free(node);
        } else {
            node_rewind(node);
        }
        node = next;
    }
    root->tree_reset = true;
}

bool evio_rx_node_rewind(evio_rx_node *top)
{
    // Queued input may target the subtree, which ignores it only while closed.
    if (top->root->queue_count || is_active(top)) {
        return false;
    }
    for (evio_rx_node *node = first_leaf(top); node;) {
        evio_rx_node *next = next_leaf(node, top);
        if (node != top && node->owns_source) {
            evio_rx_node_retire(node);
        } else {
            node_rewind(node);
        }
        node = next;
    }
    return true;
}

static void run_reopen(evio_rx_subscription *root)
{
    root->sink.closed = false;
    root->closed = false;
    root->tree_reset = false;
    root->restart = false;
    root->terminal_error = 0;
}

// A run being freed allocates nothing more, so its memory is not recycled.
static void run_free(evio_rx_subscription *root)
{
    evio_rx_node *top = root->sink.child;
    // Children go first; a fini hook does not use child nodes.
    for (evio_rx_node *node = top ? first_leaf(top) : NULL; node;) {
        evio_rx_node *next = next_leaf(node, top);
        node_fini(node);
        if (!in_arena(root, node)) {
            evio_free(node);
        }
        node = next;
    }
    evio_rx_free(root, root->queue, root->queue_capacity * sizeof(*root->queue));
    for (size_t i = 0; root->heap_blocks && i < EVIO_RX_FREE_LISTS; ++i) {
        for (void *ptr = root->free_lists[i]; ptr;) {
            void *next = *(void **)ptr;
            if (!in_arena(root, ptr)) {
                evio_free(ptr);
            }
            ptr = next;
        }
    }
    evio_rx_observable *source = root->source;
    if (source->refs > 1 && !source->spare) {
        source->spare = root;
    } else {
        evio_free(root);
    }
    evio_rx_observable_free(source);
}

static void settle_run(evio_rx_subscription *root)
{
    // Destroy hooks of freed nodes may queue input for live ones.
    root->depth++;
    for (;;) {
        if (root->queue_count && !root->closed) {
            evio_rx_message message = queue_shift(root);
            deliver(&message);
            evio_rx_buffer_fini(&message.value, root);
            continue;
        }
        while (root->queue_count) {
            evio_rx_message message = queue_shift(root);
            evio_rx_buffer_fini(&message.value, root);
        }
        if (root->terminal_pending) {
            notify_terminal(root);
            continue;
        }
        if (garbage_free(root)) {
            continue;
        }
        if (root->frames || !root->closed || root->freed) {
            break;
        }
        if (!root->tree_reset) {
            tree_reset(root);
        }
        if (!root->restart) {
            break;
        }
        // Started at the depth of the loop, so the input of the start is direct.
        run_reopen(root);
        evio_rx_node_start(root->sink.child);
    }
    root->depth--;
    root->pending = root->garbage || (root->closed && root->frames);
    if (root->freed && !root->frames) {
        run_free(root);
    }
}

void evio_rx_settle(evio_rx_subscription *root)
{
    // A closed run waits for its frames to unwind.
    if (root->queue_count || root->terminal_pending || root->garbage || !root->frames) {
        settle_run(root);
    }
}

void evio_rx_run_restart(evio_rx_subscription *root)
{
    root->restart = true;
    root->pending = true;
}

evio_rx_subscription *evio_rx_run_new(evio_loop *loop, evio_rx_observable *source,
                                      evio_rx_observer observer, size_t queue_capacity)
{
    if (queue_capacity > SIZE_MAX / sizeof(evio_rx_message)) {
        errno = EOVERFLOW;
        return NULL;
    }
    size_t arena_size = source->run_size;
    size_t size;
    if (__builtin_add_overflow(sizeof(evio_rx_subscription), arena_size, &size)) {
        arena_size = 0;
        size = sizeof(evio_rx_subscription);
    }
    evio_rx_subscription *root = source->spare;
    source->spare = NULL;
    if (root && root->arena_size >= arena_size) {
        size += root->arena_size - arena_size;
        memset(root, 0, size);
        arena_size = size - sizeof(evio_rx_subscription);
    } else {
        evio_free(root);
        root = evio_calloc(1, size);
    }
    root->sink.ops = &sink_ops;
    root->sink.root = root;
    root->observer = observer;
    if (!observer.next) {
        root->observer.next = observer_skip;
    }
    root->loop = loop;
    root->queue_capacity = queue_capacity;
    root->arena_size = arena_size;
    root->source = evio_rx_observable_ref(source);

    evio_rx_enter(root);
    if (!evio_rx_node_new(&root->sink, source, 0)) {
        int error = errno;
        root->freed = true;
        evio_rx_run_close(root);
        evio_rx_leave(root);
        errno = error;
        return NULL;
    }
    if (root->run_size > source->run_size) {
        source->run_size = root->run_size;
    }
    evio_rx_leave(root);
    return root;
}

int evio_rx_deadline(evio_loop *loop, evio_time duration, evio_time *deadline)
{
    evio_time now = evio_get_time(loop);
    if (duration > EVIO_TIME_MAX - now) {
        return EOVERFLOW;
    }
    *deadline = now + duration;
    return 0;
}

void evio_rx_timer_init(evio_rx_timer_state *timer, evio_rx_node *node)
{
    timer->node = node;
    evio_timer_init(&timer->watcher, timer_cb, 0);
    timer->watcher.data = timer;
}

void evio_rx_timer_stop(evio_rx_timer_state *timer)
{
    timer->armed = false;
    evio_timer_stop(timer->node->root->loop, &timer->watcher);
}

int evio_rx_timer_start(evio_rx_timer_state *timer, evio_time after)
{
    evio_loop *loop = timer->node->root->loop;
    int error = evio_rx_deadline(loop, after, &timer->deadline);
    if (error) {
        return error;
    }
    evio_rx_timer_stop(timer);
    timer->generation++;
    timer->armed = true;
    evio_timer_start(loop, &timer->watcher, after);
    return 0;
}
