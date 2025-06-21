#include "evio_core.h"
#include "evio_list.h"

void *evio_list_resize(void *ptr, size_t size, size_t count, size_t *total)
{
    EVIO_ASSERT(size);
    EVIO_ASSERT(count <= PTRDIFF_MAX / size);

    if (__evio_likely(*total >= count)) {
        EVIO_ASSERT(ptr);
        return ptr;
    }

    *total = 1ULL << (sizeof(unsigned long long) * 8 - __builtin_clzll(count));
    return evio_reallocarray(ptr, *total, size);
}

void evio_list_start(evio_loop *loop, evio_base *base,
                     evio_list *list, bool do_ref)
{
    if (__evio_unlikely(base->active)) {
        return;
    }

    base->active = ++list->count;

    if (do_ref) {
        evio_ref(loop);
    }

    list->ptr = evio_list_resize(list->ptr, sizeof(*list->ptr),
                                 list->count, &list->total);
    list->ptr[base->active - 1] = base;
}

void evio_list_stop(evio_loop *loop, evio_base *base,
                    evio_list *list, bool do_ref)
{
    evio_clear_pending(loop, base);

    if (__evio_unlikely(!base->active)) {
        return;
    }

    list->ptr[base->active - 1] = list->ptr[--list->count];
    list->ptr[base->active - 1]->active = base->active;

    if (do_ref) {
        evio_unref(loop);
    }

    base->active = 0;
}
