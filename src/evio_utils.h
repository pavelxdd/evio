#pragma once

/**
 * @file evio_utils.h
 * @brief A collection of utility functions and macros for the evio library.
 *
 * This includes custom assertion and abort handlers, and error string
 * formatting.
 */

#include "evio.h"

#ifndef EVIO_ASSERT
/**
 * @brief Asserts a condition, calling `evio_assert` if it fails.
 * This macro can be overridden by the user.
 */
#define EVIO_ASSERT(...) do { \
        evio_assert(__VA_ARGS__); \
    } while (0)
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
#define EVIO_ABORT(...) do { \
        evio_abort(__FILE__, __LINE__, __func__, __VA_ARGS__); \
        __builtin_unreachable(); \
    } while (0)
#endif

/**
 * @brief A callback function pointer for a custom abort handler.
 *
 * The handler can perform cleanup and logging before the program terminates.
 * @param file The source file where the abort was triggered.
 * @param line The line number in the source file.
 * @param func The function name where the abort was triggered.
 * @param format The format string for the abort message.
 * @param ap The variable arguments list for the format string.
 * @return A stream (e.g., `stderr`) for the default abort message to be
 *         written to, or `NULL` to suppress the default message.
 */
typedef FILE *(*evio_abort_cb)(const char *file, int line,
                               const char *func, const char *format, va_list ap);

/**
 * @brief Sets a custom abort handler for the entire library.
 * @param cb The callback function to be called on abort.
 */
__evio_public
void evio_set_abort(evio_abort_cb cb);

/**
 * @brief Gets the current custom abort handler.
 * @return The current abort handler callback, or NULL if not set.
 */
__evio_public __evio_nodiscard
evio_abort_cb evio_get_abort(void);

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
