#include "evio_core.h"
#include "evio_async.h"

void evio_async_start(evio_loop *loop, evio_async *w)
{
    if (__evio_unlikely(w->active)) {
        return;
    }

    evio_eventfd_init(loop);
    atomic_store_explicit(&w->status.value, 0, memory_order_release);

    evio_list_start(loop, &w->base, &loop->async, true);
}

void evio_async_stop(evio_loop *loop, evio_async *w)
{
    evio_list_stop(loop, &w->base, &loop->async, true);
}

void evio_async_send(evio_loop *loop, evio_async *w)
{
    atomic_store_explicit(&w->status.value, 1, memory_order_release);

    if (!atomic_exchange_explicit(&loop->async_pending.value, 1, memory_order_acq_rel)) {
        evio_eventfd_write(loop);
    }
}
