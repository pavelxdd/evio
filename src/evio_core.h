#pragma once

/**
 * @file evio_core.h
 * @brief Private core header (not public API).
 */

// IWYU pragma: begin_exports

#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "evio.h"
#include "evio_alloc.h"
#include "evio_heap.h"
#include "evio_list.h"
#include "evio_poll.h"
#include "evio_async.h"
#include "evio_uring.h"
#include "evio_eventfd.h"

// IWYU pragma: end_exports

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/** @brief The default initial number of events for the epoll buffer. */
#define EVIO_DEF_EVENTS ((size_t)64)
/** @brief The maximum number of events the epoll buffer can grow to. */
#define EVIO_MAX_EVENTS ((size_t)INT_MAX / sizeof(struct epoll_event))

/** @brief Internal flag combined with emask to indicate edge-triggered mode. */
#define EVIO_POLLET 0x80u
/** @brief Internal flag indicating an invalidated file descriptor. */
#define EVIO_FD_INVAL 0x80u

/** @brief A bitmask for file-descriptor flags (e.g., `EVIO_FD_INVAL`). */
typedef uint16_t evio_flag;

/** @brief A pending event to be processed. */
typedef struct {
    evio_base *base;    /**< The watcher that the event belongs to. */
    evio_mask emask;    /**< The event mask for the pending event. */
} evio_pending;

/** @brief A list of pending events, used for the double-buffered queue. */
typedef EVIO_LIST(evio_pending) evio_pending_list;

/** @brief Per-file-descriptor data. */
typedef struct {
    evio_list list;     /**< List of poll watchers for this fd. */
    size_t changes;     /**< 1-based index in the fdchanges list. */
    size_t errors;      /**< 1-based index in the fderrors list. */
    uint32_t gen;       /**< Generation counter to handle stale events. */
    evio_mask emask;    /**< The current event mask registered with epoll. */
    evio_flag flags;    /**< Flags for the fd state (e.g., `EVIO_FD_INVAL`). */
} evio_fds;

/** @brief Per-signal data. */
typedef struct {
    EVIO_ATOMIC_ALIGNED(int) status;    /**< Pending status from the signal handler. */
    _Atomic(evio_loop *) loop;          /**< The loop this signal is bound to. */
    evio_list list;                     /**< List of signal watchers for this signal. */
    struct sigaction sa_old;            /**< The original signal action. */
} evio_sig;

/* Compile-time checks for EVIO_ATOMIC size and lock-free atomics */
EVIO_ATOMIC_SIZE_CHECK(int);
EVIO_ATOMIC_ALIGNED_SIZE_CHECK(int);

EVIO_ATOMIC_LOCK_FREE_CHECK(int);
EVIO_ATOMIC_LOCK_FREE_CHECK(evio_loop *);

#define EVIO_SIGSET_WORDS (((NSIG - 1) + 63u) / 64u)

/** @brief The internal state of an event loop. */
struct evio_loop {
    int fd;                     /**< The main epoll file descriptor. */
    int done;                   /**< The loop's break state (see `EVIO_BREAK_*`). */
    int pending_queue;          /**< The index of the active pending queue (0 or 1). */
    clockid_t clock_id;         /**< The monotonic clock source ID for time functions. */
    size_t refcount;            /**< Active watcher reference count. Loop runs if > 0. */
    evio_time time;             /**< Cached monotonic time for the current iteration. */

    evio_pending_list pending[2];   /**< Double-buffered queue for pending watcher callbacks. */

    EVIO_LIST(evio_node) timer;     /**< Min-heap of active timers. */
    evio_list idle;             /**< List of active idle watchers. */
    evio_list prepare;          /**< List of active prepare watchers. */
    evio_list check;            /**< List of active check watchers. */

    EVIO_ATOMIC(int) eventfd_allow; /**< Flag to allow writing to the eventfd (thread-sync). */
    EVIO_ATOMIC(int) event_pending; /**< Flag indicating a pending eventfd notification. */
    EVIO_ATOMIC(int) async_pending; /**< Flag indicating at least one async watcher is pending. */
    EVIO_ATOMIC(int) signal_pending;/**< Flag indicating at least one signal is pending. */

    EVIO_LIST(struct epoll_event) events; /**< Buffer for epoll_wait results. */
    EVIO_LIST(evio_fds) fds;        /**< Sparse array of per-file-descriptor data. */
    EVIO_LIST(int) fdchanges;       /**< List of fds with pending epoll changes. */
    EVIO_LIST(int) fderrors;        /**< List of fds that have encountered errors. */

    evio_uring *iou;            /**< Optional io_uring context for batched epoll_ctl. */
    size_t iou_count;           /**< Number of pending io_uring operations. */

    void *data;                 /**< User-assignable data pointer. */
    evio_poll event;            /**< The internal eventfd poll watcher for loop wake-ups. */
    evio_list async;            /**< List of active async watchers. */
    evio_list cleanup;          /**< List of active cleanup watchers. */
    evio_list once;             /**< List of active once watchers. */

    sigset_t sigmask;           /**< Signal mask used in epoll_pwait to block signals. */
    uint64_t sig_active[EVIO_SIGSET_WORDS]; /**< Active signal set for this loop. */
};

/**
 * @brief Sets the pending state of a watcher.
 * @details Encode (index, queue) into `base->pending`.
 * @param base The watcher's base structure.
 * @param index The 0-based index in the pending array.
 * @param queue The queue index (0 or 1).
 */
static inline __evio_nonnull(1)
void evio_pending_set(evio_base *base, size_t index, size_t queue)
{
    base->pending = (index << 1) + 1 + queue;
}

/**
 * @brief Gets the pending array index from a watcher's pending state.
 * @param base The watcher's base structure.
 * @return The 0-based index in the pending array.
 */
static inline __evio_nonnull(1) __evio_nodiscard
size_t evio_pending_get_index(const evio_base *base)
{
    return (base->pending - 1) >> 1;
}

/**
 * @brief Gets the pending queue index from a watcher's pending state.
 * @param base The watcher's base structure.
 * @return The queue index (0 or 1).
 */
static inline __evio_nonnull(1) __evio_nodiscard
size_t evio_pending_get_queue(const evio_base *base)
{
    return (base->pending - 1) & 1;
}

/**
 * @brief Queues an event for a watcher.
 * @param loop The event loop.
 * @param base The watcher to queue the event for.
 * @param emask The event mask.
 */
static inline __evio_nonnull(1, 2)
void evio_queue_event(evio_loop *loop, evio_base *base, evio_mask emask)
{
    if (__evio_unlikely(base->pending)) {
        const size_t queue = evio_pending_get_queue(base);
        const size_t index = evio_pending_get_index(base);

        evio_pending_list *pending = &loop->pending[queue];
        EVIO_ASSERT(pending->count > index);
        EVIO_ASSERT(pending->ptr[index].base == base);

        evio_pending *p = &pending->ptr[index];
        p->emask |= emask;
        return;
    }

    const size_t queue = loop->pending_queue;
    evio_pending_list *pending = &loop->pending[queue];

    const size_t index = pending->count++;
    evio_pending_set(base, index, queue);

    pending->ptr = evio_list_ensure(pending->ptr, sizeof(evio_pending),
                                    pending->count, &pending->total);

    evio_pending *p = &pending->ptr[index];
    p->base = base;
    p->emask = emask;
}

/**
 * @brief Queues the same event for multiple watchers.
 * @param loop The event loop.
 * @param base An array of watcher base pointers.
 * @param count The number of watchers in the array.
 * @param emask The event mask.
 */
__evio_nonnull(1, 2) __evio_hot
void evio_queue_events(evio_loop *loop, evio_base **base, size_t count, evio_mask emask);

/**
 * @brief Queues events for all watchers on a file descriptor.
 * @param loop The event loop.
 * @param fd The file descriptor.
 * @param emask The event mask.
 */
__evio_nonnull(1) __evio_hot
void evio_queue_fd_events(evio_loop *loop, int fd, evio_mask emask);

/**
 * @brief Queues a change notification for a file descriptor.
 * @param loop The event loop.
 * @param fd The file descriptor.
 * @param flags The change flags.
 */
__evio_nonnull(1)
void evio_queue_fd_change(evio_loop *loop, int fd, evio_flag flags);

/**
 * @brief Queues an error notification for a file descriptor.
 * @param loop The event loop.
 * @param fd The file descriptor.
 */
__evio_nonnull(1)
void evio_queue_fd_error(evio_loop *loop, int fd);

/**
 * @brief Queues error events for all watchers on a file descriptor.
 * @param loop The event loop.
 * @param fd The file descriptor.
 */
__evio_nonnull(1)
void evio_queue_fd_errors(evio_loop *loop, int fd);

/**
 * @brief Flushes a pending file descriptor change from the queue.
 * @param loop The event loop.
 * @param idx The index of the change to flush.
 */
__evio_nonnull(1)
void evio_flush_fd_change(evio_loop *loop, int idx);

/**
 * @brief Flushes a pending file descriptor error from the queue.
 * @param loop The event loop.
 * @param idx The index of the error to flush.
 */
__evio_nonnull(1)
void evio_flush_fd_error(evio_loop *loop, int idx);

/**
 * @brief Invalidates a file descriptor, preparing it for removal from epoll.
 * @param loop The event loop.
 * @param fd The file descriptor to invalidate.
 * @return 0 on success, -1 on error.
 */
__evio_nonnull(1)
int evio_invalidate_fd(evio_loop *loop, int fd);

/**
 * @brief Queues signal events for all watchers on a given signal number.
 * @param loop The event loop.
 * @param signum The signal number.
 */
__evio_nonnull(1)
void evio_signal_queue_events(evio_loop *loop, int signum);

/**
 * @brief Processes pending signal events delivered via the eventfd.
 * @param loop The event loop.
 */
__evio_nonnull(1)
void evio_signal_process_pending(evio_loop *loop);

/**
 * @brief Cleans up all signal handlers associated with a loop.
 * @param loop The event loop.
 */
__evio_nonnull(1)
void evio_signal_cleanup_loop(evio_loop *loop);

/**
 * @brief Updates file descriptor watchers in the event loop via epoll_ctl.
 * @param loop The event loop.
 */
__evio_nonnull(1) __evio_hot
void evio_poll_update(evio_loop *loop);

/**
 * @brief Waits for I/O events.
 * @param loop The event loop.
 * @param timeout The timeout in milliseconds.
 */
__evio_nonnull(1) __evio_hot
void evio_poll_wait(evio_loop *loop, int timeout);

/**
 * @brief Updates timer watchers in the event loop, firing expired timers.
 * @param loop The event loop.
 */
__evio_nonnull(1) __evio_hot
void evio_timer_update(evio_loop *loop);
