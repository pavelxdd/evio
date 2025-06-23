#pragma once

/**
 * @file evio_utils.h
 * @brief A collection of utility functions and macros for the `evio` library.
 *
 * This includes customizable assertion and abort handlers, and error string
 * formatting.
 */

#include "evio.h"

#ifndef EVIO_ASSERT
/**
 * @brief Asserts a condition, calling `evio_assert` if it fails.
 * This macro can be overridden by the user.
 */
#define EVIO_ASSERT(...) evio_assert(__VA_ARGS__)
#endif

#ifndef evio_assert
/**
 * @brief The default assertion implementation, using the standard `assert`.
 * This macro can be overridden by the user.
 */
#define evio_assert(...) assert(__VA_ARGS__)
#endif

#ifndef EVIO_ABORT
/**
 * @brief Triggers a fatal error, printing a message and terminating the program.
 * @details This macro calls the backing `evio_abort` function with file, line,
 * and function information, followed by a user-provided format string and
 * arguments.
 * @param ... A printf-style format string and optional arguments. If empty,
 *            no custom message is printed.
 */
#define EVIO_ABORT(...) evio_abort(__FILE__, __LINE__, __func__, __VA_ARGS__)
#endif

/**
 * @brief A callback function pointer for a custom abort handler.
 *
 * The handler is called by `EVIO_ABORT` and can perform cleanup before the
 * program terminates.
 * @param ctx The user-defined context pointer provided to `evio_set_abort`.
 * @return A `FILE*` stream (e.g., `stderr`) to which the default abort message
 *         will be written. If `NULL`, the default message is suppressed.
 */
typedef FILE *(*evio_abort_cb)(void *ctx);

/**
 * @brief Sets a custom abort handler for the entire library.
 * This function is not thread-safe and should be called before any other evio
 * functions are used.
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
 * @details This is intended for testing purposes to override the default
 * `abort()` behavior.
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
 * @details This macro allocates a temporary buffer on the stack to store the
 * error message returned by `evio_strerror`.
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
