#include <errno.h>

#include "evio_rx_node.h"
#include "evio_rx_buffer.h"
#include "evio_alloc.h"

typedef struct evio_rx_subject_node evio_rx_subject_node;

struct evio_rx_subject {
    evio_loop *loop;
    const evio_rx_type *type;
    evio_rx_ring queue;
    evio_rx_subject_node *first;
    evio_rx_subject_node *last;
    evio_rx_subject_node *cursor;   /**< The next subscriber of the current delivery. */
    evio_rx_subject_node *end;      /**< The last subscriber of the current delivery. */
    size_t refs;
    int error;
    bool busy;
    bool closed;
    bool terminal_sent;
};

typedef struct {
    evio_rx_observable base;
    evio_rx_subject *subject;
} evio_rx_subject_observable;

struct evio_rx_subject_node {
    evio_rx_node base;
    evio_rx_subject_node *previous;
    evio_rx_subject_node *next;
    size_t skip;                    /**< Queued values published before the node joined. */
    bool linked;
};

void evio_rx_subject_free(evio_rx_subject *subject)
{
    if (--subject->refs) {
        return;
    }
    evio_rx_ring_fini(&subject->queue, NULL);
    evio_free(subject);
}

static void subject_start(evio_rx_node *base)
{
    evio_rx_subject_observable *source = (evio_rx_subject_observable *)base->source;
    evio_rx_subject *subject = source->subject;
    if (subject->closed) {
        evio_rx_emit_end(base, subject->error);
        return;
    }
    evio_rx_subject_node *node = (evio_rx_subject_node *)base;
    node->previous = subject->last;
    node->next = NULL;
    if (subject->last) {
        subject->last->next = node;
    } else {
        subject->first = node;
    }
    subject->last = node;
    node->skip = subject->queue.count;
    node->linked = true;
}

static void subject_stop(evio_rx_node *base)
{
    evio_rx_subject_node *node = (evio_rx_subject_node *)base;
    if (!node->linked) {
        return;
    }
    evio_rx_subject_observable *source = (evio_rx_subject_observable *)base->source;
    evio_rx_subject *subject = source->subject;
    if (subject->cursor == node) {
        subject->cursor = node == subject->end ? NULL : node->next;
    }
    if (subject->end == node) {
        subject->end = node->previous;
    }
    if (node->previous) {
        node->previous->next = node->next;
    } else {
        subject->first = node->next;
    }
    if (node->next) {
        node->next->previous = node->previous;
    } else {
        subject->last = node->previous;
    }
    node->linked = false;
}

static void subject_observable_free(evio_rx_observable *base)
{
    evio_rx_subject_observable *source = (evio_rx_subject_observable *)base;
    evio_rx_subject_free(source->subject);
    evio_free(source);
}

static const evio_rx_ops subject_ops = {
    .node_size = sizeof(evio_rx_subject_node),
    .start = subject_start,
    .stop = subject_stop,
    .free = subject_observable_free,
};

evio_rx_subject *evio_rx_subject_new(evio_loop *loop, const evio_rx_type *type, size_t capacity)
{
    if (!loop || evio_rx_type_check(type)) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_ring queue;
    int error = evio_rx_ring_init(&queue, type, capacity, NULL);
    if (error) {
        errno = error;
        return NULL;
    }
    evio_rx_subject *subject = evio_calloc(1, sizeof(*subject));
    subject->loop = loop;
    subject->type = type;
    subject->queue = queue;
    subject->refs = 1;
    return subject;
}

evio_rx_observable *evio_rx_from_subject(evio_rx_subject *subject)
{
    if (!subject) {
        errno = EINVAL;
        return NULL;
    }
    evio_rx_subject_observable *source = evio_calloc(1, sizeof(*source));
    evio_rx_observable_init(&source->base, &subject_ops, subject->type,
                            subject->loop, (evio_rx_capture) {0});
    source->subject = subject;
    subject->refs++;
    return &source->base;
}

// Subscribers that join during a delivery do not receive it.
static void fan_out(evio_rx_subject *subject, const void *value)
{
    subject->end = subject->last;
    for (evio_rx_subject_node *node = subject->first; node; node = subject->cursor) {
        subject->cursor = node == subject->end ? NULL : node->next;
        (void)evio_rx_emit(&node->base, value);
    }
    subject->end = NULL;
}

static void fan_out_batch(evio_rx_subject *subject, const void *values, size_t count)
{
    subject->end = subject->last;
    for (evio_rx_subject_node *node = subject->first; node; node = subject->cursor) {
        subject->cursor = node == subject->end ? NULL : node->next;
        (void)evio_rx_emit_batch(&node->base, values, count);
    }
    subject->end = NULL;
}

static void fan_out_queued(evio_rx_subject *subject, const void *value)
{
    subject->end = subject->last;
    for (evio_rx_subject_node *node = subject->first; node; node = subject->cursor) {
        subject->cursor = node == subject->end ? NULL : node->next;
        if (node->skip) {
            node->skip--;
        } else {
            (void)evio_rx_emit(&node->base, value);
        }
    }
    subject->end = NULL;
}

static void fan_out_end(evio_rx_subject *subject)
{
    subject->end = subject->last;
    for (evio_rx_subject_node *node = subject->first; node; node = subject->cursor) {
        subject->cursor = node == subject->end ? NULL : node->next;
        evio_rx_emit_end(&node->base, subject->error);
    }
    subject->end = NULL;
}

static void flush(evio_rx_subject *subject)
{
    while (subject->queue.count && !subject->error) {
        void *value = evio_rx_ring_shift(&subject->queue);
        fan_out_queued(subject, value);
        evio_rx_value_destroy(subject->type, value);
    }
    if (subject->error) {
        evio_rx_ring_clear(&subject->queue);
    }
    if (subject->closed && !subject->terminal_sent) {
        subject->terminal_sent = true;
        fan_out_end(subject);
    }
}

// Queues a nested or invalid publication.
static int publish_queued(evio_rx_subject *subject, const void *values, size_t count)
{
    subject->refs++;
    int error = values ? 0 : EINVAL;
    const char *value = values;
    for (size_t i = 0; i < count && !error; ++i, value += subject->type->size) {
        error = evio_rx_ring_push(&subject->queue, value);
    }
    if (error) {
        subject->closed = true;
        subject->error = error;
    }
    if (!subject->busy) {
        subject->busy = true;
        flush(subject);
        subject->busy = false;
    }
    evio_rx_subject_free(subject);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int evio_rx_subject_next(evio_rx_subject *subject, const void *value)
{
    if (subject->closed) {
        errno = EPIPE;
        return -1;
    }
    if (!value || subject->busy) {
        return publish_queued(subject, value, 1);
    }
    subject->refs++;
    subject->busy = true;
    fan_out(subject, value);
    if (subject->queue.count || subject->closed) {
        flush(subject);
    }
    subject->busy = false;
    evio_rx_subject_free(subject);
    return 0;
}

int evio_rx_subject_next_batch(evio_rx_subject *subject, const void *values, size_t count)
{
    if (subject->closed) {
        errno = EPIPE;
        return -1;
    }
    if (!count) {
        return 0;
    }
    if (!values || subject->busy) {
        return publish_queued(subject, values, count);
    }
    subject->refs++;
    subject->busy = true;
    fan_out_batch(subject, values, count);
    if (subject->queue.count || subject->closed) {
        flush(subject);
    }
    subject->busy = false;
    evio_rx_subject_free(subject);
    return 0;
}

static void terminate(evio_rx_subject *subject, int error)
{
    if (subject->closed) {
        return;
    }
    subject->refs++;
    subject->closed = true;
    subject->error = error;
    if (!subject->busy) {
        subject->busy = true;
        flush(subject);
        subject->busy = false;
    }
    evio_rx_subject_free(subject);
}

void evio_rx_subject_error(evio_rx_subject *subject, int error)
{
    terminate(subject, error > 0 ? error : EINVAL);
}

void evio_rx_subject_complete(evio_rx_subject *subject)
{
    terminate(subject, 0);
}

// Outside a delivery the queue is drained and the terminal event is sent.
int evio_rx_subject_reopen(evio_rx_subject *subject)
{
    if (subject->busy) {
        errno = EBUSY;
        return -1;
    }
    subject->closed = false;
    subject->terminal_sent = false;
    subject->error = 0;
    return 0;
}
