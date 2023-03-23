/*
 * Copyright (c) 2023 Pavel Otchertsov <pavel.otchertsov@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

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
#   if __evio_has_attribute(__nonnull__)
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

#ifndef __evio_inline
#   if __evio_has_attribute(__always_inline__)
#       define __evio_inline inline __attribute__((__always_inline__))
#   else
#       define __evio_inline inline
#   endif
#endif

typedef void *(*evio_realloc_cb)(void *ctx, void *ptr, size_t size);

__evio_public
void evio_set_allocator(evio_realloc_cb cb, void *ctx);

enum {
    EVIO_MASK       = 0xFFF,
    EVIO_NONE       = 0x000,
    EVIO_READ       = 0x001,
    EVIO_WRITE      = 0x002,
    EVIO_POLL       = 0x004,
    EVIO_TIMER      = 0x008,
    EVIO_SIGNAL     = 0x010,
    EVIO_ASYNC      = 0x020,
    EVIO_IDLE       = 0x040,
    EVIO_PREPARE    = 0x080,
    EVIO_CHECK      = 0x100,
    EVIO_CLEANUP    = 0x200,
    EVIO_WALK       = 0x400,
    EVIO_ERROR      = 0x800,
};

enum {
    EVIO_RUN_DEFAULT    = 0,
    EVIO_RUN_NOWAIT     = 1,
    EVIO_RUN_ONCE       = 2,
};

enum {
    EVIO_BREAK_CANCEL   = 0,
    EVIO_BREAK_ONE      = 1,
    EVIO_BREAK_ALL      = 2,
};

typedef struct evio_loop    evio_loop;
typedef struct evio_base    evio_base;
typedef struct evio_list    evio_list;
typedef struct evio_poll    evio_poll;
typedef struct evio_timer   evio_timer;
typedef struct evio_signal  evio_signal;
typedef struct evio_async   evio_async;
typedef struct evio_idle    evio_idle;
typedef struct evio_prepare evio_prepare;
typedef struct evio_check   evio_check;
typedef struct evio_cleanup evio_cleanup;

typedef void (*evio_cb)(evio_loop *loop, evio_base *base, uint16_t emask);

#define EVIO_COMMON \
    size_t active; \
    size_t pending; \
    void *data; \
    evio_cb cb; \

#define EVIO_BASE \
    union { \
        evio_base base; \
        struct { \
            EVIO_COMMON \
        }; \
    }

#define EVIO_LIST \
    union { \
        evio_base base; \
        evio_list list; \
        struct { \
            EVIO_COMMON \
            evio_list *next; \
        }; \
    }

struct evio_base {
    EVIO_COMMON
};

struct evio_list {
    EVIO_BASE;
    evio_list *next;
};

// Create a new event loop.
__evio_public __evio_nodiscard
evio_loop *evio_loop_new(int maxevents);

// Destroy an event loop.
__evio_public __evio_nonnull(1)
void evio_loop_free(evio_loop *loop);

// Reinitialize the event loop state after the fork.
__evio_public __evio_nonnull(1)
void evio_loop_fork(evio_loop *loop);

// Get the monotonic time (in milliseconds) from the event loop.
__evio_public __evio_nonnull(1) __evio_nodiscard
uint64_t evio_time(evio_loop *loop);

// Update the monotonic time in the event loop.
__evio_public __evio_nonnull(1)
void evio_update_time(evio_loop *loop);

// Add a reference count to the event loop.
__evio_public __evio_nonnull(1)
void evio_ref(evio_loop *loop);

// Remove a reference count from the event loop.
__evio_public __evio_nonnull(1)
void evio_unref(evio_loop *loop);

// Get the current reference count from the event loop.
__evio_public __evio_nonnull(1) __evio_nodiscard
size_t evio_refcount(evio_loop *loop);

// Associate arbitrary user data with an event loop.
__evio_public __evio_nonnull(1)
void evio_set_userdata(evio_loop *loop, void *data);

// Retrieve arbitrary user data from the event loop.
__evio_public __evio_nonnull(1) __evio_nodiscard
void *evio_get_userdata(evio_loop *loop);

// Start handling events in a loop.
__evio_public __evio_nonnull(1)
int evio_run(evio_loop *loop, uint8_t flags);

// Stop handling events in a loop.
__evio_public __evio_nonnull(1)
void evio_break(evio_loop *loop, uint8_t state);

// Walk through event loop watchers.
__evio_public __evio_nonnull(1, 2)
void evio_walk(evio_loop *loop, evio_cb cb, uint16_t emask);

// Send an event to the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_feed_event(evio_loop *loop, evio_base *base, uint16_t emask);

// Send a file descriptor event to the event loop.
__evio_public __evio_nonnull(1)
void evio_feed_fd_event(evio_loop *loop, int fd, uint8_t emask);

// Send a file descriptor error to the event loop.
__evio_public __evio_nonnull(1)
void evio_feed_fd_error(evio_loop *loop, int fd);

// Send a global signal event.
__evio_public
void evio_feed_signal(int signum);

// Send a signal event to the event loop.
__evio_public __evio_nonnull(1)
void evio_feed_signal_event(evio_loop *loop, int signum);

// Send a signal error to the event loop.
__evio_public __evio_nonnull(1)
void evio_feed_signal_error(evio_loop *loop, int signum);

// Invoke pending event handlers in an event loop.
__evio_public __evio_nonnull(1)
void evio_invoke_pending(evio_loop *loop);

// Clear the pending event status in an event loop watcher.
__evio_public __evio_nonnull(1, 2)
void evio_clear_pending(evio_loop *loop, evio_base *base);

// Get the current pending event count from the event loop.
__evio_public __evio_nonnull(1) __evio_nodiscard
size_t evio_pending_count(evio_loop *loop);

// Initialize a base watcher.
static __evio_inline __evio_nonnull(1, 2)
void evio_init(evio_base *base, evio_cb cb)
{
    base->active = 0;
    base->pending = 0;
    base->cb = cb;
}

// ####################################################################
// EVIO_POLL
// ####################################################################

struct evio_poll {
    EVIO_LIST;
    int fd;
    uint8_t emask;
};

// Modify the poll watcher events mask.
static __evio_inline __evio_nonnull(1)
void evio_poll_modify(evio_poll *w, uint8_t emask)
{
    w->emask = (emask & (EVIO_READ | EVIO_WRITE)) | (w->emask & EVIO_POLL);
}

// Modify the poll watcher file descriptor and events mask.
static __evio_inline __evio_nonnull(1)
void evio_poll_set(evio_poll *w, int fd, uint8_t emask)
{
    w->fd = fd;
    w->emask = (emask & (EVIO_READ | EVIO_WRITE)) | EVIO_POLL;
}

// Initialize a poll watcher with a file descriptor and an events mask.
static __evio_inline __evio_nonnull(1, 2)
void evio_poll_init(evio_poll *w, evio_cb cb, int fd, uint8_t emask)
{
    evio_init(&w->base, cb);
    evio_poll_set(w, fd, emask);
}

// Add poll watcher to the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_poll_start(evio_loop *loop, evio_poll *w);

// Remove poll watcher from the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_poll_stop(evio_loop *loop, evio_poll *w);

// ####################################################################
// EVIO_TIMER
// ####################################################################

struct evio_timer {
    EVIO_BASE;
    uint64_t time;
    uint64_t repeat;
};

// Modify timer watcher timeout and period (in milliseconds).
static __evio_inline __evio_nonnull(1)
void evio_timer_set(evio_timer *w, uint64_t after, uint64_t repeat)
{
    w->time = after;
    w->repeat = repeat;
}

// Initialize a timer watcher with a timeout and period (in milliseconds).
static __evio_inline __evio_nonnull(1, 2)
void evio_timer_init(evio_timer *w, evio_cb cb, uint64_t after, uint64_t repeat)
{
    evio_init(&w->base, cb);
    evio_timer_set(w, after, repeat);
}

// Add timer watcher to the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_timer_start(evio_loop *loop, evio_timer *w);

// Remove timer watcher from the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_timer_stop(evio_loop *loop, evio_timer *w);

// Restart a repeating timer watcher.
__evio_public __evio_nonnull(1, 2)
void evio_timer_again(evio_loop *loop, evio_timer *w);

// Get the current timeout (in milliseconds) from the timer watcher.
__evio_public __evio_nonnull(1, 2) __evio_nodiscard
uint64_t evio_timer_remaining(evio_loop *loop, evio_timer *w);

// ####################################################################
// EVIO_SIGNAL
// ####################################################################

struct evio_signal {
    EVIO_LIST;
    int signum;
};

// Modify a signal watcher signal number.
static __evio_inline __evio_nonnull(1)
void evio_signal_set(evio_signal *w, int signum)
{
    w->signum = signum;
}

// Initialize a signal watcher with a signal number.
static __evio_inline __evio_nonnull(1, 2)
void evio_signal_init(evio_signal *w, evio_cb cb, int signum)
{
    evio_init(&w->base, cb);
    evio_signal_set(w, signum);
}

// Add signal watcher to the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_signal_start(evio_loop *loop, evio_signal *w);

// Remove signal watcher from the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_signal_stop(evio_loop *loop, evio_signal *w);

// ####################################################################
// EVIO_ASYNC
// ####################################################################

struct evio_async {
    EVIO_BASE;
    _Atomic uint8_t status;
};

// Get the current pending event status from the async watcher.
static __evio_inline __evio_nonnull(1) __evio_nodiscard
int evio_async_pending(evio_async *w)
{
    return atomic_load_explicit(&w->status, memory_order_acquire);
}

// Initialize an async watcher.
static __evio_inline __evio_nonnull(1, 2)
void evio_async_init(evio_async *w, evio_cb cb)
{
    evio_init(&w->base, cb);
    atomic_init(&w->status, 0);
}

// Add async watcher to the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_async_start(evio_loop *loop, evio_async *w);

// Remove async watcher from the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_async_stop(evio_loop *loop, evio_async *w);

// Signal an async event to event loop.
__evio_public __evio_nonnull(1, 2)
void evio_async_send(evio_loop *loop, evio_async *w);

// ####################################################################
// EVIO_IDLE
// ####################################################################

struct evio_idle {
    EVIO_BASE;
};

// Initialize an idle watcher.
static __evio_inline __evio_nonnull(1, 2)
void evio_idle_init(evio_idle *w, evio_cb cb)
{
    evio_init(&w->base, cb);
}

// Add idle watcher to the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_idle_start(evio_loop *loop, evio_idle *w);

// Remove idle watcher from the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_idle_stop(evio_loop *loop, evio_idle *w);

// ####################################################################
// EVIO_PREPARE
// ####################################################################

struct evio_prepare {
    EVIO_BASE;
};

// Initialize a prepare watcher.
static __evio_inline __evio_nonnull(1, 2)
void evio_prepare_init(evio_prepare *w, evio_cb cb)
{
    evio_init(&w->base, cb);
}

// Add prepare watcher to the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_prepare_start(evio_loop *loop, evio_prepare *w);

// Remove prepare watcher from the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_prepare_stop(evio_loop *loop, evio_prepare *w);

// ####################################################################
// EVIO_CHECK
// ####################################################################

struct evio_check {
    EVIO_BASE;
};

// Initialize a check watcher.
static __evio_inline __evio_nonnull(1, 2)
void evio_check_init(evio_check *w, evio_cb cb)
{
    evio_init(&w->base, cb);
}

// Add check watcher to the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_check_start(evio_loop *loop, evio_check *w);

// Remove check watcher from the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_check_stop(evio_loop *loop, evio_check *w);

// ####################################################################
// EVIO_CLEANUP
// ####################################################################

struct evio_cleanup {
    EVIO_BASE;
};

// Initialize a cleanup watcher.
static __evio_inline __evio_nonnull(1, 2)
void evio_cleanup_init(evio_cleanup *w, evio_cb cb)
{
    evio_init(&w->base, cb);
}

// Add cleanup watcher to the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_cleanup_start(evio_loop *loop, evio_cleanup *w);

// Remove cleanup watcher from the event loop.
__evio_public __evio_nonnull(1, 2)
void evio_cleanup_stop(evio_loop *loop, evio_cleanup *w);

// ####################################################################

typedef union evio_watcher {
    evio_base base;
    evio_list list;
    evio_poll poll;
    evio_timer timer;
    evio_signal signal;
    evio_async async;
    evio_idle idle;
    evio_prepare prepare;
    evio_check check;
    evio_cleanup cleanup;
} evio_watcher;

#undef EVIO_COMMON
#undef EVIO_BASE
#undef EVIO_LIST

#ifdef __cplusplus
}
#endif
