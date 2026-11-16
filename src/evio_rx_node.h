#pragma once

/**
 * @file evio_rx_node.h
 * @brief Internal Rx runtime: run nodes, delivery and run memory.
 *
 * A subscription instantiates its description as a tree of nodes. A node
 * notifies its parent by a direct call; the parent of the entry node is the
 * subscription sink, which notifies the observer.
 *
 * Input from outside the tree enters through `evio_rx_emit()`. It is delivered
 * directly when no delivery of the run is on the stack and no node is being
 * started, or when it comes from the node being started and none of that node's
 * own deliveries is on the stack.
 * Any other input, including input from user code called inside a delivery, is
 * copied into the run FIFO and delivered once the stack unwinds.
 *
 * Closing a node closes its subtree, so an open node has an open parent. Node
 * memory is released only when no delivery is on the stack: a retired subtree
 * once none of its nodes is marked active by an enclosing start or emitter
 * frame, the tree of a closed run once no runtime frame is on the stack.
 */

#include "evio_rx.h"

typedef struct evio_rx_node evio_rx_node;
typedef struct evio_rx_message evio_rx_message;

/** @brief Node behaviour of a description. */
typedef struct {
    size_t node_size;
    /** Creates children and per-run state; may fail with a positive errno. */
    int (*init)(evio_rx_node *node);
    /** Starts children or production; may deliver synchronously. */
    void (*start)(evio_rx_node *node);
    /** Detaches external callbacks; runs once, after the subtree is closed. */
    void (*stop)(evio_rx_node *node);
    /** Restores the state after init once the run is closed and unused; children go first. */
    void (*reset)(evio_rx_node *node);
    /** Releases per-run state of a successful init. */
    void (*fini)(evio_rx_node *node);
    /** Handles a value of child `input`. */
    void (*next)(evio_rx_node *node, evio_rx_node *input, const void *value);
    /** Handles `count` > 1 contiguous values of child `input`; NULL delivers them one by one. */
    void (*next_batch)(evio_rx_node *node, evio_rx_node *input, const void *values, size_t count);
    /** Handles the terminal event of child `input`; `error` is 0 on completion. */
    void (*end)(evio_rx_node *node, evio_rx_node *input, int error);
    /** Handles the expiry of the node's timer. */
    void (*tick)(evio_rx_node *node);
    /** Releases the description. */
    void (*free)(evio_rx_observable *source);
} evio_rx_ops;

struct evio_rx_observable {
    const evio_rx_ops *ops;
    const evio_rx_type *type;
    evio_loop *loop;            /**< Required loop, or NULL. */
    evio_rx_capture capture;
    size_t refs;
    size_t node_size;           /**< `ops->node_size` and inline per-run storage. */
    size_t run_size;            /**< Run memory of a run of this entry before start. */
    evio_rx_subscription *spare; /**< The memory of a freed run of this entry, or NULL. */
};

struct evio_rx_node {
    const evio_rx_ops *ops;
    evio_rx_observable *source;
    evio_rx_subscription *root;
    evio_rx_node *parent;
    evio_rx_node *child;        /**< The newest child. */
    evio_rx_node *prev;         /**< A newer sibling. */
    evio_rx_node *next;         /**< An older sibling, or the next retired subtree. */
    size_t index;               /**< Position among the parent's inputs. */
    size_t active;              /**< Frames outside deliveries that use the node across a settle. */
    bool owns_source;
    bool ready;                 /**< Init succeeded. */
    bool started;
    bool stopped;
    bool input_closed;          /**< Accepts no input; set until start. */
    bool closed;                /**< Emits nothing more. */
};

/**
 * @brief A node timer.
 * @details A tick queued while the run is busy is dropped if the timer was
 * stopped or restarted before delivery.
 */
typedef struct {
    evio_timer watcher;
    evio_rx_node *node;
    evio_time deadline;
    uint64_t generation;
    bool armed;
} evio_rx_timer_state;

struct evio_rx_emitter {
    evio_rx_node base;
    void *state;
    bool prepared;
};

/** @brief The maximum number of values a synchronous source delivers at once. */
#define EVIO_RX_BATCH 64

/** @brief The number of run memory free lists, one per size in `max_align_t` units. */
#define EVIO_RX_FREE_LISTS 16

struct evio_rx_subscription {
    evio_rx_node sink;          /**< The parent of the entry node. */
    evio_rx_observer observer;
    evio_loop *loop;
    evio_rx_observable *source; /**< The entry description, held until free. */
    evio_rx_node *producer;     /**< The node being started or invoked. */
    size_t gate;                /**< `depth` at which `producer` delivers directly. */
    size_t depth;               /**< Deliveries on the stack. */
    size_t frames;              /**< Runtime frames that use nodes after a delivery. */
    evio_rx_node *garbage;      /**< Retired subtrees linked through `next`. */
    evio_rx_message *queue;
    size_t queue_capacity;
    size_t queue_head;
    size_t queue_count;
    void *free_lists[EVIO_RX_FREE_LISTS];
    size_t heap_blocks;         /**< Run memory blocks taken from the heap. */
    size_t run_size;            /**< Run memory requested before start. */
    size_t arena_size;
    size_t arena_used;
    int terminal_error;
    bool started;
    bool closed;
    bool tree_reset;            /**< The closed tree is back in its state after init. */
    bool restart;               /**< Start again once the closed tree is reset. */
    bool freed;
    bool terminal_pending;
    bool pending;               /**< Queued input, a terminal, garbage or closed run work. */
    /** Run memory sized by the runs of the entry description before start. */
    alignas(max_align_t) unsigned char arena[];
};

/**
 * @brief Allocates zeroed run memory.
 * @param size The size, nonzero.
 */
void *evio_rx_alloc(evio_rx_subscription *root, size_t size);

/** @brief Releases run memory of `evio_rx_alloc()` with the same size; `ptr` may be NULL. */
void evio_rx_free(evio_rx_subscription *root, void *ptr, size_t size);

/**
 * @brief Initializes a description with one reference.
 * @param loop The loop required by the description, or NULL.
 */
void evio_rx_observable_init(evio_rx_observable *source, const evio_rx_ops *ops,
                             const evio_rx_type *type, evio_loop *loop, evio_rx_capture capture);

/**
 * @brief Creates a run with an initialized entry node.
 * @param queue_capacity Positive capacity of the run FIFO.
 * @return The run, or NULL with errno set.
 */
evio_rx_subscription *evio_rx_run_new(evio_loop *loop, evio_rx_observable *source,
                                      evio_rx_observer observer, size_t queue_capacity);

/** @brief Closes the run and its tree without a terminal notification. */
void evio_rx_run_close(evio_rx_subscription *root);

/** @brief Closes the run with an error; a non-positive code becomes EINVAL. */
void evio_rx_fail(evio_rx_subscription *root, int error);

/** @brief Requests a new start once the closed tree is reset; the caller holds a frame. */
void evio_rx_run_restart(evio_rx_subscription *root);

/**
 * @brief Delivers queued input and the terminal notification of an idle run;
 * outside runtime frames also resets or frees a closed run and restarts it.
 */
void evio_rx_settle(evio_rx_subscription *root);

/** @brief Enters a runtime frame that uses nodes after a delivery. */
static inline __evio_nonnull(1)
void evio_rx_enter(evio_rx_subscription *root)
{
    root->frames++;
}

/** @brief Leaves a runtime frame; may free the run. */
static inline __evio_nonnull(1)
void evio_rx_leave(evio_rx_subscription *root)
{
    root->frames--;
    if (root->pending && !root->depth) {
        evio_rx_settle(root);
    }
}

/**
 * @brief Creates and initializes a child node.
 * @details The node borrows `source`, which must outlive it. A node whose init
 * failed stays in the tree until the run is freed.
 * @return The node, or NULL with errno set.
 */
evio_rx_node *evio_rx_node_new(evio_rx_node *parent, evio_rx_observable *source, size_t index);

/** @brief Like `evio_rx_node_new()`, but the node takes the reference `source` in any case. */
evio_rx_node *evio_rx_node_adopt(evio_rx_node *parent, evio_rx_observable *source, size_t index);

/** @brief Starts an open node once, as the producer of its synchronous input. */
void evio_rx_node_start(evio_rx_node *node);

/** @brief Closes a subtree, then stops its children first. */
void evio_rx_node_cancel(evio_rx_node *node);

/** @brief Cancels a subtree and moves it out of the tree for release. */
void evio_rx_node_retire(evio_rx_node *node);

/**
 * @brief Prepares a cancelled subtree for a new start in place.
 * @details Retires the subtrees it created while running.
 * @return `false` if a runtime frame or queued input may still use the subtree.
 */
bool evio_rx_node_rewind(evio_rx_node *top);

/** @brief Cancels an open node and notifies its parent of the terminal event. */
void evio_rx_node_finish(evio_rx_node *node, int error);

/** @brief Forwards the terminal event of a child; an `end` handler. */
void evio_rx_forward_end(evio_rx_node *node, evio_rx_node *input, int error);

/** @brief Notifies the parent of an open node of a value. */
static inline __evio_nonnull(1, 2)
void evio_rx_node_next(evio_rx_node *node, const void *value)
{
    if (node->closed) {
        return;
    }
    evio_rx_node *parent = node->parent;
    evio_rx_subscription *root = node->root;
    if (parent == &root->sink) {
        root->observer.next(root->observer.ctx, value);
        return;
    }
    parent->ops->next(parent, node, value);
}

/** @brief Notifies the parent of an open node of `count` > 0 contiguous values. */
static inline __evio_nonnull(1)
void evio_rx_node_next_batch(evio_rx_node *node, const void *values, size_t count)
{
    if (count == 1) {
        evio_rx_node_next(node, values);
        return;
    }
    evio_rx_node *parent = node->parent;
    evio_rx_subscription *root = node->root;
    if (parent != &root->sink && parent->ops->next_batch) {
        if (!node->closed) {
            parent->ops->next_batch(parent, node, values, count);
        }
        return;
    }
    size_t size = node->source->type->size;
    const char *value = values;
    for (size_t i = 0; i < count && !node->closed; ++i, value += size) {
        evio_rx_node_next(node, value);
    }
}

/** @brief Handles input that cannot be delivered directly, see `evio_rx_emit()`. */
bool evio_rx_emit_slow(evio_rx_node *node, const void *value);

/** @brief Delivers values one by one, see `evio_rx_emit_batch()`. */
bool evio_rx_emit_each(evio_rx_node *node, const void *values, size_t count);

/** @brief Settles the run after a direct delivery of an input node. */
bool evio_rx_emit_settle(evio_rx_node *node);

/**
 * @brief Delivers a value of an input node from outside the tree.
 * @details A NULL value fails the run with EINVAL.
 * @return `false` if the node accepts no more input.
 */
static inline __evio_nonnull(1)
bool evio_rx_emit(evio_rx_node *node, const void *value)
{
    evio_rx_subscription *root = node->root;
    if (node->input_closed || !value || root->queue_count || root->depth != root->gate ||
        (root->producer && root->producer != node)) {
        return evio_rx_emit_slow(node, value);
    }
    root->depth++;
    evio_rx_node_next(node, value);
    root->depth--;
    if (root->pending && !root->depth) {
        return evio_rx_emit_settle(node);
    }
    return !node->input_closed;
}

/**
 * @brief Delivers `count` contiguous values of an input node from outside the tree.
 * @details Input queued during the batch is delivered after it.
 * @return `false` if the node accepts no more input.
 */
static inline __evio_nonnull(1, 2)
bool evio_rx_emit_batch(evio_rx_node *node, const void *values, size_t count)
{
    evio_rx_subscription *root = node->root;
    if (count == 1) {
        return evio_rx_emit(node, values);
    }
    if (node->input_closed || root->queue_count || root->depth != root->gate ||
        (root->producer && root->producer != node)) {
        return evio_rx_emit_each(node, values, count);
    }
    root->depth++;
    evio_rx_node_next_batch(node, values, count);
    root->depth--;
    if (root->pending && !root->depth) {
        return evio_rx_emit_settle(node);
    }
    return !node->input_closed;
}

/**
 * @brief Delivers the terminal event of an input node from outside the tree.
 * @details Stops the node at once; queued values are delivered first.
 */
void evio_rx_emit_end(evio_rx_node *node, int error);

/**
 * @brief Computes `now + duration` on the loop clock.
 * @return 0, or EOVERFLOW.
 */
int evio_rx_deadline(evio_loop *loop, evio_time duration, evio_time *deadline);

/** @brief Initializes a stopped timer that ticks `node`. */
void evio_rx_timer_init(evio_rx_timer_state *timer, evio_rx_node *node);

/**
 * @brief Restarts the timer.
 * @return 0, or EOVERFLOW.
 */
int evio_rx_timer_start(evio_rx_timer_state *timer, evio_time after);

/** @brief Stops the timer and invalidates its queued ticks. */
void evio_rx_timer_stop(evio_rx_timer_state *timer);
