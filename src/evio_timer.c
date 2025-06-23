#include "evio_core.h"
#include "evio_timer.h"

void evio_timer_start(evio_loop *loop, evio_timer *w, evio_time after)
{
    if (__evio_unlikely(w->active)) {
        return;
    }

    evio_time time = loop->time + after;
    if (__evio_unlikely(time < loop->time)) {
        return;
    }

    w->active = ++loop->timer.count;
    evio_ref(loop);

    loop->timer.ptr = evio_list_resize(loop->timer.ptr, sizeof(*loop->timer.ptr),
                                       loop->timer.count, &loop->timer.total);

    evio_node *node = &loop->timer.ptr[w->active - 1];
    node->base = &w->base;
    node->time = time;

    evio_heap_up(loop->timer.ptr, w->active - 1);
}

void evio_timer_stop(evio_loop *loop, evio_timer *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active)) {
        return;
    }

    size_t count = --loop->timer.count;

    if (w->active <= count) {
        loop->timer.ptr[w->active - 1] = loop->timer.ptr[count];
        evio_heap_adjust(loop->timer.ptr, w->active - 1, count);
    }

    evio_unref(loop);
    w->active = 0;
}

void evio_timer_again(evio_loop *loop, evio_timer *w)
{
    evio_clear_pending(loop, &w->base);

    if (w->active) {
        if (!w->repeat || __evio_unlikely(loop->time >= EVIO_TIME_MAX - w->repeat)) {
            evio_timer_stop(loop, w);
        } else {
            loop->timer.ptr[w->active - 1].time = loop->time + w->repeat;
            evio_heap_adjust(loop->timer.ptr, w->active - 1, loop->timer.count);
        }
    } else if (w->repeat) {
        evio_timer_start(loop, w, w->repeat);
    }
}

evio_time evio_timer_remaining(const evio_loop *loop, const evio_timer *w)
{
    if (!w->active) {
        return 0;
    }

    EVIO_ASSERT(w->active <= loop->timer.count);

    evio_node *node = &loop->timer.ptr[w->active - 1];
    if (node->time <= loop->time) {
        return 0;
    }

    return node->time - loop->time;
}

void evio_timer_update(evio_loop *loop)
{
    while (
        loop->timer.count &&
        loop->timer.ptr[0].time <= loop->time
    ) {
        evio_node *node = &loop->timer.ptr[0];
        evio_timer *w = container_of(node->base, evio_timer, base);

        evio_queue_event(loop, &w->base, EVIO_TIMER);

        if (!w->repeat || __evio_unlikely(node->time >= EVIO_TIME_MAX - w->repeat)) {
            // One-shot timer: remove from heap WITHOUT clearing pending event.
            size_t count = --loop->timer.count;
            evio_unref(loop);
            w->active = 0;

            if (count) {
                loop->timer.ptr[0] = loop->timer.ptr[count];
                evio_heap_down(loop->timer.ptr, 0, count);
            }
        } else {
            // Repeating timer: reschedule.
            node->time += w->repeat;
            if (node->time <= loop->time) {
                node->time = loop->time + 1;
            }
            evio_heap_down(loop->timer.ptr, 0, loop->timer.count);
        }
    }
}
