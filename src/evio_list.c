#include "evio_core.h"
#include "evio_list.h"

void *evio_list_resize(void *ptr, size_t size, size_t count, size_t *total)
{
    if (__evio_likely(*total >= count)) {
        EVIO_ASSERT(ptr);
        return ptr;
    }

    if (__evio_unlikely(count > PTRDIFF_MAX)) {
        *total = count;
    } else {
        *total = 1ULL << (sizeof(unsigned long long) * 8 - __builtin_clzll(count));
    }

    return evio_reallocarray(ptr, *total, size);
}

void evio_list_start(evio_loop *loop, evio_base *w,
                     evio_list *list, bool do_ref)
{
    if (__evio_unlikely(w->active)) {
        return;
    }

    w->active = ++list->count;

    if (do_ref) {
        evio_ref(loop);
    }

    list->ptr = evio_list_resize(list->ptr, sizeof(*list->ptr),
                                 list->count, &list->total);
    list->ptr[w->active - 1] = w;
}

void evio_list_stop(evio_loop *loop, evio_base *w,
                    evio_list *list, bool do_ref)
{
    evio_clear_pending(loop, w);

    if (__evio_unlikely(!w->active)) {
        return;
    }

    list->ptr[w->active - 1] = list->ptr[--list->count];
    list->ptr[w->active - 1]->active = w->active;

    if (do_ref) {
        evio_unref(loop);
    }

    w->active = 0;
}
