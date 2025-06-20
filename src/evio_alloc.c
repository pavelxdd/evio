#include "evio_core.h"
#include "evio_alloc.h"

/**
 * @brief The default realloc-like implementation used by the library.
 * @param ctx Unused context pointer.
 * @param ptr The memory block to reallocate or free.
 * @param size The new size. If 0, the block is freed.
 * @return A pointer to the allocated memory, or NULL if size is 0.
 */
static void *evio_default_realloc(void *ctx, void *ptr, size_t size)
{
    if (size) {
        return realloc(ptr, size);
    }

    free(ptr);
    return NULL;
}

static struct {
    evio_realloc_cb cb;
    void *ctx;
} evio_allocator = {
    .cb = evio_default_realloc,
    .ctx = NULL,
};

void evio_set_allocator(evio_realloc_cb cb, void *ctx)
{
    evio_allocator.cb = cb ? cb : evio_default_realloc;
    evio_allocator.ctx = ctx;
}

evio_realloc_cb evio_get_allocator(void **ctx)
{
    if (ctx) {
        *ctx = evio_allocator.ctx;
    }
    return evio_allocator.cb;
}

void *evio_malloc(size_t size)
{
    void *ptr = evio_allocator.cb(evio_allocator.ctx, NULL, size);
    if (__evio_unlikely(!ptr)) {
        EVIO_ABORT("Allocation failed\n");
    }
    return ptr;
}

void *evio_calloc(size_t n, size_t size)
{
    size_t total;
    if (__evio_unlikely(__builtin_mul_overflow(n, size, &total))) {
        EVIO_ABORT("Integer overflow\n");
    }
    return memset(evio_malloc(total), 0, total);
}

void *evio_realloc(void *ptr, size_t size)
{
    ptr = evio_allocator.cb(evio_allocator.ctx, ptr, size);
    if (__evio_unlikely(!ptr)) {
        EVIO_ABORT("Reallocation failed\n");
    }
    return ptr;
}

void *evio_reallocarray(void *ptr, size_t n, size_t size)
{
    size_t total;
    if (__evio_unlikely(__builtin_mul_overflow(n, size, &total))) {
        EVIO_ABORT("Integer overflow\n");
    }
    return evio_realloc(ptr, total);
}

void evio_free(void *ptr)
{
    evio_allocator.cb(evio_allocator.ctx, ptr, 0);
}
