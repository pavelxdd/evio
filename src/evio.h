#pragma once

/**
 * @file evio.h
 * @brief The main public header for the `evio` library.
 *
 * An application should include this single file to access all public API
 * features. It aggregates all other public headers and defines the core types,
 * enumerations, and base structures that form the foundation of the library.
 */

// IWYU pragma: begin_exports

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <assert.h>
#include <signal.h>
#include <time.h>

// IWYU pragma: end_exports

#ifndef __evio_has_attribute
#   ifdef __has_attribute
#       define __evio_has_attribute(x) __has_attribute(x)
#   else
#       define __evio_has_attribute(x) (0)
#   endif
#endif

#ifndef __evio_public
#   if __evio_has_attribute(__visibility__)
#       define __evio_public __attribute__((__visibility__("default")))
#   else
#       define __evio_public
#   endif
#endif

#ifndef __evio_nonnull
#   if __evio_has_attribute(__nonnull__) && !defined(EVIO_TESTING)
#       define __evio_nonnull(...) __attribute__((__nonnull__(__VA_ARGS__)))
#   else
#       define __evio_nonnull(...)
#   endif
#endif

#ifndef __evio_nodiscard
#   if __evio_has_attribute(__warn_unused_result__)
#       define __evio_nodiscard __attribute__((__warn_unused_result__))
#   else
#       define __evio_nodiscard
#   endif
#endif

#ifndef __evio_returns_nonnull
#   if __evio_has_attribute(__returns_nonnull__)
#       define __evio_returns_nonnull __attribute__((__returns_nonnull__))
#   else
#       define __evio_returns_nonnull
#   endif
#endif

#ifndef __evio_format_printf
#   if __evio_has_attribute(__format__)
#       define __evio_format_printf(f, a) __attribute__((__format__(__printf__, f, a)))
#   else
#       define __evio_format_printf(f, a)
#   endif
#endif

#ifndef __evio_noreturn
#   if __evio_has_attribute(__noreturn__)
#       define __evio_noreturn __attribute__((__noreturn__))
#   else
#       define __evio_noreturn
#   endif
#endif

#ifndef __evio_has_builtin
#   ifdef __has_builtin
#       define __evio_has_builtin(x) __has_builtin(x)
#   else
#       define __evio_has_builtin(x) (0)
#   endif
#endif

#ifndef __evio_likely
#   if __evio_has_builtin(__builtin_expect)
#       define __evio_likely(x) (__builtin_expect(!!(x), 1))
#   else
#       define __evio_likely(x) (x)
#   endif
#endif

#ifndef __evio_unlikely
#   if __evio_has_builtin(__builtin_expect)
#       define __evio_unlikely(x) (__builtin_expect(!!(x), 0))
#   else
#       define __evio_unlikely(x) (x)
#   endif
#endif

// IWYU pragma: begin_exports

#include "evio_version.h"
#include "evio_utils.h"
#include "evio_alloc.h"

// IWYU pragma: end_exports

/**
 * @brief A bitmask for watcher events (e.g., `EVIO_READ`, `EVIO_TIMER`).
 *
 * This type is used throughout the library in callbacks and watcher setups to
 * specify which events are being handled or have occurred.
 */
typedef uint16_t evio_mask;

/** @brief Event mask values for watchers. */
enum {
    EVIO_NONE       = 0x000, /**< No event. */
    EVIO_READ       = 0x001, /**< Read readiness on a file descriptor. */
    EVIO_WRITE      = 0x002, /**< Write readiness on a file descriptor. */
    EVIO_POLL       = 0x004, /**< A poll event occurred (internal use). */
    EVIO_TIMER      = 0x008, /**< A timer has expired. */
    EVIO_SIGNAL     = 0x010, /**< A signal has been received. */
    EVIO_ASYNC      = 0x020, /**< An async event has been sent. */
    EVIO_IDLE       = 0x040, /**< The loop is idle. */
    EVIO_PREPARE    = 0x080, /**< A prepare phase event. */
    EVIO_CHECK      = 0x100, /**< A check phase event. */
    EVIO_CLEANUP    = 0x200, /**< A cleanup phase event. */
    EVIO_ONCE       = 0x400, /**< A one-shot poll or timer event occurred. */
    EVIO_ERROR      = 0x800, /**< An error occurred on a watcher. */
};

/** @brief Flags for `evio_loop_new` to customize loop creation. */
enum {
    EVIO_FLAG_NONE  = 0x000, /**< Default flags. */
    EVIO_FLAG_URING = 0x001, /**< Use io_uring to optimize `epoll_ctl` syscalls if available. */
};

/** @brief Flags for `evio_run` to control loop execution. */
enum {
    EVIO_RUN_DEFAULT    = 0, /**< Run until stopped or no active watchers remain. */
    EVIO_RUN_NOWAIT     = 1, /**< Run one iteration, but do not block for I/O. */
    EVIO_RUN_ONCE       = 2, /**< Run one iteration and block for I/O if needed. */
};

/** @brief Break states for `evio_break` to control loop termination. */
enum {
    EVIO_BREAK_CANCEL   = 0, /**< Cancel a previous break request. */
    EVIO_BREAK_ONE      = 1, /**< Stop the current `evio_run` call. */
    EVIO_BREAK_ALL      = 2, /**< Stop the current and all nested `evio_run` calls. */
};

#ifndef EVIO_CACHELINE
/** @brief Defines the cache line size for aligning atomic variables. */
#define EVIO_CACHELINE 64
#endif

/**
 * @brief Creates a cache-line aligned atomic variable to prevent false sharing.
 * @param type The atomic type (e.g., int, bool).
 */
#define EVIO_ATOMIC(type) struct { \
        alignas(EVIO_CACHELINE) _Atomic(type) value; /**< The atomic value. */ \
        char _pad[EVIO_CACHELINE - sizeof(_Atomic(type))]; /**< Padding to prevent false sharing. */ \
    }

/** @brief Represents time in nanoseconds, stored as a 64-bit unsigned integer. */
typedef uint64_t evio_time;

/** @brief The maximum value for evio_time. */
#define EVIO_TIME_MAX            UINT64_MAX
/** @brief Creates an evio_time constant. */
#define EVIO_TIME_C(c)           UINT64_C(c)
/** @brief Casts a value to evio_time. */
#define EVIO_TIME(t)             (evio_time)(t)

/** @brief Nanoseconds per microsecond. */
#define EVIO_TIME_PER_USEC       EVIO_TIME_C(1000)
/** @brief Nanoseconds per millisecond. */
#define EVIO_TIME_PER_MSEC       EVIO_TIME_C(1000000)
/** @brief Nanoseconds per second. */
#define EVIO_TIME_PER_SEC        EVIO_TIME_C(1000000000)

/** @brief Converts microseconds to nanoseconds. */
#define EVIO_TIME_FROM_USEC(t)   (EVIO_TIME(t) * EVIO_TIME_PER_USEC)
/** @brief Converts milliseconds to nanoseconds. */
#define EVIO_TIME_FROM_MSEC(t)   (EVIO_TIME(t) * EVIO_TIME_PER_MSEC)
/** @brief Converts seconds to nanoseconds. */
#define EVIO_TIME_FROM_SEC(t)    (EVIO_TIME(t) * EVIO_TIME_PER_SEC)

/** @brief Converts nanoseconds to microseconds. */
#define EVIO_TIME_TO_USEC(t)     (EVIO_TIME(t) / EVIO_TIME_PER_USEC)
/** @brief Converts nanoseconds to milliseconds. */
#define EVIO_TIME_TO_MSEC(t)     (EVIO_TIME(t) / EVIO_TIME_PER_MSEC)
/** @brief Converts nanoseconds to seconds. */
#define EVIO_TIME_TO_SEC(t)      (EVIO_TIME(t) / EVIO_TIME_PER_SEC)

/** @brief An opaque type representing an event loop instance. */
typedef struct evio_loop evio_loop;
/** @brief An opaque type representing the base of any watcher. */
typedef struct evio_base evio_base;

/**
 * @brief A generic callback type for all watcher events.
 * @param loop The event loop that triggered the event.
 * @param base A pointer to the watcher's base structure.
 * @param emask A bitmask of the events that occurred.
 */
typedef void (*evio_cb)(evio_loop *loop, evio_base *base, evio_mask emask);

/**
 * @brief Common fields for all watcher base structures.
 * @details This macro defines the core members required by the event loop to
 * manage a watcher. It should be included via the `EVIO_BASE` macro.
 */
#define EVIO_COMMON \
    size_t active;  /**< Non-zero if active, 0 otherwise. */ \
    size_t pending; /**< Non-zero if pending, 0 otherwise. */ \
    void *data;     /**< User-assignable data pointer. */ \
    evio_cb cb;     /**< Watcher's callback function. */

/**
 * @brief Defines the base structure for all watcher types.
 * @details This macro should be the first member of any watcher struct. It uses
 * a union to allow polymorphic access via an `evio_base*` pointer.
 */
#define EVIO_BASE \
    union { \
        struct evio_base base; /**< Allows casting to the base struct type. */ \
        struct { \
            EVIO_COMMON \
        }; \
    }

/**
 * @brief A lightweight base structure embedded in every watcher type.
 *
 * The event loop treats all concrete watcher kinds polymorphically through an
 * `evio_base *`. The `active` and `pending` fields are one-based indices used
 * internally to manage watcher lists and the pending-event queue. `data` is a
 * user-assignable pointer, and `cb` stores the callback that the loop invokes
 * when the watcher is triggered.
 */
struct evio_base {
    EVIO_COMMON
};

/**
 * @brief Initializes the base properties of a watcher.
 * @param base The watcher base to initialize.
 * @param cb The callback function to be invoked for this watcher.
 */
static inline __evio_nonnull(1, 2)
void evio_init(evio_base *base, evio_cb cb)
{
    EVIO_ASSERT(cb);
    base->active = 0;
    base->pending = 0;
    base->cb = cb;
}

/**
 * @brief Invokes a watcher's callback with the given event mask.
 * @param loop The event loop.
 * @param base The watcher whose callback should be invoked.
 * @param emask The event mask to pass to the callback.
 */
static inline __evio_nonnull(1, 2)
void evio_invoke(evio_loop *loop, evio_base *base, evio_mask emask)
{
    EVIO_ASSERT(base->cb);
    base->cb(loop, base, emask);
}

// IWYU pragma: begin_exports

#include "evio_loop.h"
#include "evio_poll.h"
#include "evio_timer.h"
#include "evio_signal.h"
#include "evio_async.h"
#include "evio_idle.h"
#include "evio_prepare.h"
#include "evio_check.h"
#include "evio_cleanup.h"
#include "evio_once.h"

// IWYU pragma: end_exports
