#pragma once

/**
 * @file evio_core.h
 * @brief The central private header for the `evio` library.
 *
 * It aggregates all other internal headers and defines the core data structures,
 * such as `evio_loop`, that represent the event loop's state. It also declares
 * the internal functions that form the backbone of event processing, file
 * descriptor management, and watcher updates. This header is not part of the
 * public API.
 */

// IWYU pragma: begin_exports

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#include "evio.h"
#include "evio_alloc.h"
#include "evio_heap.h"
#include "evio_list.h"
#include "evio_poll.h"
#include "evio_async.h"
#include "evio_uring.h"
#include "evio_eventfd.h"

// IWYU pragma: end_exports

/** @brief The default initial number of events for the epoll buffer. */
#define EVIO_DEF_EVENTS ((size_t)64)
/** @brief The maximum number of events the epoll buffer can grow to. */
#define EVIO_MAX_EVENTS ((size_t)INT_MAX / sizeof(struct epoll_event))

/** @brief Internal flag combined with emask to indicate edge-triggered mode. */
#define EVIO_POLLET 0x80u
/** @brief Internal flag indicating an invalidated file descriptor. */
#define EVIO_FD_INVAL 0x80u

/** @brief A pending event to be processed. */
typedef struct {
    evio_base *base;    /**< The watcher that the event belongs to. */
    evio_mask emask;    /**< The event mask for the pending event. */
} evio_pending;

/** @brief Per-file-descriptor data. */
typedef struct {
    evio_list list;     /**< List of poll watchers for this fd. */
    size_t changes;     /**< 1-based index in the fdchanges list. */
    size_t errors;      /**< 1-based index in the fderrors list. */
    uint32_t gen;       /**< Generation counter to handle stale events. */
    uint8_t emask;      /**< The current event mask registered with epoll. */
    uint8_t cache;      /**< The previous event mask, for change detection. */
    uint8_t flags;      /**< Flags for the fd state (e.g., `EVIO_FD_INVAL`). */
} evio_fds;

/** @brief Per-signal data. */
typedef struct {
    EVIO_ATOMIC(int) status;    /**< Pending status from the signal handler. */
    _Atomic(evio_loop *) loop;  /**< The loop this signal is bound to. */
    evio_list list;             /**< List of signal watchers for this signal. */
    struct sigaction sa_old;    /**< The original signal action. */
} evio_sig;

/** @brief The event loop state. */
struct evio_loop {
    evio_uring *iou;            /**< Optional io_uring context. */
    size_t iou_count;           /**< Number of pending io_uring operations. */

    void *data;                 /**< User-assignable data pointer. */
    size_t refcount;            /**< Active watcher reference count. */

    evio_time time;             /**< Cached monotonic time for the current iteration. */
    clockid_t clock_id;         /**< The monotonic clock source ID. */

    int fd;                     /**< The main epoll file descriptor. */
    int done;                   /**< The loop's break state. */

    evio_poll event;            /**< The internal eventfd poll watcher. */

    EVIO_ATOMIC(int) eventfd_allow; /**< Flag to allow writing to the eventfd. */
    EVIO_ATOMIC(int) event_pending; /**< Flag indicating a pending eventfd notification. */
    EVIO_ATOMIC(int) async_pending; /**< Flag indicating pending async events. */
    EVIO_ATOMIC(int) signal_pending;/**< Flag indicating pending signal events. */

    EVIO_LIST(evio_pending) pending;/**< Queue of pending events for the current iteration. */
    EVIO_LIST(evio_fds) fds;        /**< Array of per-file-descriptor data. */
    EVIO_LIST(int) fdchanges;       /**< List of fds with pending changes. */
    EVIO_LIST(int) fderrors;        /**< List of fds with pending errors. */
    EVIO_LIST(evio_node) timer;     /**< Min-heap of active timers. */

    evio_list idle;             /**< List of active idle watchers. */
    evio_list async;            /**< List of active async watchers. */
    evio_list prepare;          /**< List of active prepare watchers. */
    evio_list check;            /**< List of active check watchers. */
    evio_list cleanup;          /**< List of active cleanup watchers. */
    evio_list once;             /**< List of active once watchers. */

    EVIO_LIST(struct epoll_event) events; /**< Buffer for epoll_wait results. */

    sigset_t sigmask;           /**< Signal mask used in epoll_pwait. */
};

/**
 * @brief Queues an event for a watcher.
 * @param loop The event loop.
 * @param base The watcher to queue the event for.
 * @param emask The event mask.
 */
__evio_nonnull(1, 2)
void evio_queue_event(evio_loop *loop, evio_base *base, evio_mask emask);

/**
 * @brief Queues the same event for multiple watchers.
 * @param loop The event loop.
 * @param base An array of watcher base pointers.
 * @param count The number of watchers in the array.
 * @param emask The event mask.
 */
__evio_nonnull(1, 2)
void evio_queue_events(evio_loop *loop, evio_base **base, size_t count, evio_mask emask);

/**
 * @brief Queues events for all watchers on a file descriptor.
 * @param loop The event loop.
 * @param fd The file descriptor.
 * @param emask The event mask.
 */
__evio_nonnull(1)
void evio_queue_fd_events(evio_loop *loop, int fd, evio_mask emask);

/**
 * @brief Queues a change notification for a file descriptor.
 * @param loop The event loop.
 * @param fd The file descriptor.
 * @param flags The change flags.
 */
__evio_nonnull(1)
void evio_queue_fd_change(evio_loop *loop, int fd, uint8_t flags);

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
__evio_nonnull(1)
void evio_poll_update(evio_loop *loop);

/**
 * @brief Waits for I/O events.
 * @param loop The event loop.
 * @param timeout The timeout in milliseconds.
 */
__evio_nonnull(1)
void evio_poll_wait(evio_loop *loop, int timeout);

/**
 * @brief Updates timer watchers in the event loop, firing expired timers.
 * @param loop The event loop.
 */
__evio_nonnull(1)
void evio_timer_update(evio_loop *loop);
