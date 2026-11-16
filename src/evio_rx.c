#include <errno.h>

#include "evio_rx_node.h"
#include "evio_alloc.h"

evio_rx_observable *evio_rx_observable_ref(evio_rx_observable *source)
{
    source->refs++;
    return source;
}

void evio_rx_observable_free(evio_rx_observable *source)
{
    if (--source->refs) {
        return;
    }
    evio_free(source->spare);
    evio_rx_capture capture = source->capture;
    source->ops->free(source);
    if (capture.destroy) {
        capture.destroy(capture.data);
    }
}

evio_rx_subscription *evio_rx_subscription_new(evio_loop *loop, evio_rx_observable *source,
                                               evio_rx_observer observer, size_t queue_capacity)
{
    if (!loop || !source || (source->loop && source->loop != loop) || !queue_capacity) {
        errno = EINVAL;
        return NULL;
    }
    return evio_rx_run_new(loop, source, observer, queue_capacity);
}

void evio_rx_subscription_start(evio_rx_subscription *subscription)
{
    if (subscription->started || subscription->closed) {
        return;
    }
    evio_rx_enter(subscription);
    subscription->started = true;
    evio_rx_node_start(subscription->sink.child);
    evio_rx_leave(subscription);
}

void evio_rx_subscription_restart(evio_rx_subscription *subscription)
{
    evio_rx_enter(subscription);
    subscription->terminal_pending = false;
    subscription->started = true;
    evio_rx_run_close(subscription);
    evio_rx_run_restart(subscription);
    evio_rx_leave(subscription);
}

void evio_rx_subscription_cancel(evio_rx_subscription *subscription)
{
    evio_rx_enter(subscription);
    subscription->terminal_pending = false;
    subscription->restart = false;
    evio_rx_run_close(subscription);
    evio_rx_leave(subscription);
}

bool evio_rx_subscription_is_closed(const evio_rx_subscription *subscription)
{
    return subscription->closed;
}

void evio_rx_subscription_free(evio_rx_subscription *subscription)
{
    evio_rx_enter(subscription);
    subscription->freed = true;
    subscription->terminal_pending = false;
    subscription->pending = true;
    evio_rx_run_close(subscription);
    evio_rx_leave(subscription);
}
