#pragma once

/**
 * @file evio_utils.h
 * @brief Utility macros and hooks.
 */

#include "evio.h"

#ifndef EVIO_ASSERT
/**
 * @brief Asserts a condition, calling `evio_assert` if it fails.
 */
#define EVIO_ASSERT(...) evio_assert(__VA_ARGS__)
#endif

#ifndef evio_assert
/**
 * @brief The default assertion implementation, using the standard `assert`.
 */
#define evio_assert(...) assert(__VA_ARGS__)
#endif

#ifndef EVIO_ABORT
/**
 * @brief Triggers a fatal error, printing a message and terminating the program.
 * @details Calls `evio_abort(__FILE__, __LINE__, __func__, ...)`.
 * @param ... A printf-style format string and optional arguments.
 *            To print no custom message, pass an empty format string: `EVIO_ABORT("")`.
 */
#define EVIO_ABORT(...) evio_abort(__FILE__, __LINE__, __func__, __VA_ARGS__)
#endif

/**
 * @brief A callback function pointer for a custom abort handler.
 * @param ctx The user-defined context pointer provided to `evio_set_abort`.
 * @return Output stream for the default abort message, or NULL to suppress it.
 */
typedef FILE *(*evio_abort_cb)(void *ctx);

/**
 * @brief Sets a custom abort handler for the entire library.
 * @details Not thread-safe; call before using evio.
 * @param cb The callback function to be called on abort.
 * @param ctx A user-defined context pointer to be passed to the callback.
 */
__evio_public
void evio_set_abort(evio_abort_cb cb, void *ctx);

/**
 * @brief Gets the current custom abort handler.
 * @param[out] ctx If not NULL, the user-defined context pointer is stored here.
 * @return The current abort handler callback, or NULL if not set.
 */
__evio_public __evio_nodiscard
evio_abort_cb evio_get_abort(void **ctx);

/**
 * @brief Backing implementation for the EVIO_ABORT macro. Do not call directly.
 * @param file The source file.
 * @param line The line number.
 * @param func The function name.
 * @param format The format string.
 * @param ... The arguments for the format string.
 */
__evio_public __evio_nonnull(1, 3, 4)
__evio_noreturn __evio_format_printf(4, 5)
void evio_abort(const char *restrict file, int line,
                const char *restrict func, const char *restrict format, ...);

/**
 * @brief Overrides the program termination function.
 * @details Intended for tests.
 * @param func The function to call instead of `abort()`.
 *             If NULL, `abort()` is restored.
 */
__evio_public
void evio_set_abort_func(void (*func)(void));

/**
 * @brief Gets the current program termination function.
 * @return The function pointer that will be called to terminate the program.
 */
__evio_public __evio_nodiscard __evio_returns_nonnull
void (*evio_get_abort_func(void))(void);

#ifndef EVIO_STRERROR_SIZE
/** @brief The default buffer size for the `EVIO_STRERROR` macro. */
#define EVIO_STRERROR_SIZE 128
#endif

#ifndef EVIO_STRERROR
/**
 * @brief A convenient macro to get an error string from an error code.
 * @details Uses a stack buffer with automatic storage duration (valid until the
 * end of the current scope).
 * @param err The error number.
 * @return A pointer to the stack-allocated string.
 */
#define EVIO_STRERROR(err) \
    evio_strerror(err, (char[EVIO_STRERROR_SIZE]){ 0 }, EVIO_STRERROR_SIZE)
#endif

/**
 * @brief Returns a string describing the given error code.
 * @param err The error number.
 * @param data A buffer to store the error string.
 * @param size The size of the buffer.
 * @return A pointer to the buffer `data`.
 */
__evio_public __evio_nodiscard
char *evio_strerror(int err, char *data, size_t size);
