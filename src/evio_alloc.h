#pragma once

/**
 * @file evio_alloc.h
 * @brief Allocator hooks and wrappers.
 */

#include "evio.h"

/**
 * @brief A callback function pointer for a custom memory allocator.
 * @details Must behave like `realloc(ptr, size)`.
 * @param ctx The user-defined context pointer provided to `evio_set_allocator`.
 * @param ptr The pointer to the memory block to reallocate or free. Can be `NULL`.
 * @param size The new size of the memory block in bytes. Can be `0`.
 * @return A pointer to the allocated memory, or `NULL` if `size` is `0`.
 */
typedef void *(*evio_realloc_cb)(void *ctx, void *ptr, size_t size);

/**
 * @brief Sets a custom memory allocator for the entire library.
 * @details Not thread-safe; call before using evio.
 * @param cb The realloc-like callback function. If NULL, defaults to `realloc`.
 * @param ctx A user-defined context pointer to be passed to the callback.
 */
__evio_public
void evio_set_allocator(evio_realloc_cb cb, void *ctx);

/**
 * @brief Gets the current custom memory allocator.
 * @param[out] ctx If not NULL, the user-defined context pointer is stored here.
 * @return The current realloc-like callback function.
 */
__evio_public __evio_nodiscard __evio_returns_nonnull
evio_realloc_cb evio_get_allocator(void **ctx);

/**
 * @brief Allocates `size` bytes of memory. Aborts on failure.
 * @pre `size > 0`. Passing 0 is a programming error and will trigger an assertion.
 * @param size The number of bytes to allocate.
 * @return A pointer to the allocated memory.
 */
__evio_public __evio_nodiscard __evio_returns_nonnull
void *evio_malloc(size_t size);

/**
 * @brief Allocates memory for an array of `n` elements of `size` bytes each
 * and initializes it to zero. Aborts on failure.
 * @pre `n > 0 && size > 0`. Passing 0 for either is a programming error.
 * @param n The number of elements.
 * @param size The size of each element.
 * @return A pointer to the allocated memory.
 */
__evio_public __evio_nodiscard __evio_returns_nonnull
void *evio_calloc(size_t n, size_t size);

/**
 * @brief Changes the size of the memory block pointed to by `ptr`.
 * Aborts on failure.
 * @pre `size > 0`. Passing 0 is a programming error (use `evio_free` instead).
 * @param ptr The pointer to the memory block to resize.
 * @param size The new size in bytes.
 * @return A pointer to the reallocated memory.
 */
__evio_public __evio_nodiscard __evio_returns_nonnull
void *evio_realloc(void *ptr, size_t size);

/**
 * @brief Changes the size of the memory block for an array.
 * Aborts on failure or integer overflow.
 * @pre `n > 0 && size > 0`. Passing 0 for either is a programming error.
 * @param ptr The pointer to the memory block to resize.
 * @param n The new number of elements.
 * @param size The size of each element.
 * @return A pointer to the reallocated memory.
 */
__evio_public __evio_nodiscard __evio_returns_nonnull
void *evio_reallocarray(void *ptr, size_t n, size_t size);

/**
 * @brief Frees the memory space pointed to by `ptr`.
 * @param ptr The pointer to the memory to free.
 */
__evio_public
void evio_free(void *ptr);
