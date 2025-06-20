#pragma once

#include_next <assert.h> // IWYU pragma: export

extern void mock_assert(const int result, const char *const expr,
                        const char *const file, const int line);

/**
 * This helper is used to wrap mock_assert and provide a hint to the static
 * analyzer that the function does not return on failure, without creating
 * a branch that causes coverage issues.
 */
static inline void evio_mock_assert(const int result, const char *const expr,
                                    const char *const file, const int line)
{
    mock_assert(result, expr, file, line);
    /* GCOVR_EXCL_START */
    if (!result) {
        __builtin_unreachable();
    }
    /* GCOVR_EXCL_STOP */
}

#undef assert
#define assert(expr) evio_mock_assert(!!(expr), #expr, __FILE__, __LINE__)
