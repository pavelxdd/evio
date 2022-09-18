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

#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

#ifndef __evio_has_builtin
#   ifdef __has_builtin
#       define __evio_has_builtin(x) __has_builtin(x)
#   else
#       define __evio_has_builtin(x) (0)
#   endif
#endif

#ifndef __evio_has_attribute
#   ifdef __has_attribute
#       define __evio_has_attribute(x) __has_attribute(x)
#   else
#       define __evio_has_attribute(x) (0)
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

typedef uint64_t evio_time_t;

#define EVIO_TIME_FROM_SEC(t)   ((evio_time_t)((t) * 1000000000ull))
#define EVIO_TIME_FROM_MSEC(t)  ((evio_time_t)((t) * 1000000ull))
#define EVIO_TIME_FROM_USEC(t)  ((evio_time_t)((t) * 1000ull))
#define EVIO_TIME_FROM_NSEC(t)  ((evio_time_t)(t))
#define EVIO_TIME_FROM_MIN(t)   EVIO_TIME_FROM_SEC((t) * 60ull)
#define EVIO_TIME_FROM_HOUR(t)  EVIO_TIME_FROM_MIN((t) * 60ull)
#define EVIO_TIME_FROM_DAY(t)   EVIO_TIME_FROM_HOUR((t) * 24ull)

#define EVIO_TIME_TO_SEC(t)     ((evio_time_t)((t) / 1000000000ull))
#define EVIO_TIME_TO_MSEC(t)    ((evio_time_t)((t) / 1000000ull))
#define EVIO_TIME_TO_USEC(t)    ((evio_time_t)((t) / 1000ull))
#define EVIO_TIME_TO_NSEC(t)    ((evio_time_t)(t))
#define EVIO_TIME_TO_MIN(t)     (EVIO_TIME_TO_SEC(t) / 60ull)
#define EVIO_TIME_TO_HOUR(t)    (EVIO_TIME_TO_MIN(t) / 60ull)
#define EVIO_TIME_TO_DAY(t)     (EVIO_TIME_TO_HOUR(t) / 24ull)

static __evio_inline __evio_nodiscard
struct timespec evio_timespec(clockid_t clock_id)
{
    struct timespec ts;
    if (__evio_unlikely(clock_gettime(clock_id, &ts)))
        abort();
    return ts;
}

__evio_public __evio_nodiscard
struct timespec evio_timespec_realtime(void);

__evio_public __evio_nodiscard
struct timespec evio_timespec_monotonic(void);

static __evio_inline __evio_nodiscard
evio_time_t evio_timestamp(struct timespec ts)
{
    return EVIO_TIME_FROM_SEC(ts.tv_sec) +
           EVIO_TIME_FROM_NSEC(ts.tv_nsec);
}

static __evio_inline __evio_nodiscard
evio_time_t evio_clocktime(clockid_t clock_id)
{
    return evio_timestamp(evio_timespec(clock_id));
}

static __evio_inline __evio_nodiscard
evio_time_t evio_time(void)
{
    return evio_timestamp(evio_timespec_realtime());
}

static __evio_inline __evio_nodiscard
evio_time_t evio_monotonic_time(void)
{
    return evio_timestamp(evio_timespec_monotonic());
}

static __evio_inline
void evio_nanosleep(struct timespec ts)
{
    for (;;) {
        int err = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts);
        if (__evio_likely(!err))
            return;
        if (__evio_unlikely(err != EINTR))
            abort();
    }
}

static __evio_inline
void evio_sleep(evio_time_t delay)
{
    struct timespec ts;
    ts.tv_sec = EVIO_TIME_TO_SEC(delay);
    ts.tv_nsec = delay - EVIO_TIME_FROM_SEC(ts.tv_sec);
    evio_nanosleep(ts);
}

typedef void *(*evio_realloc_cb)(void *ctx, void *ptr, size_t size);

__evio_public
void evio_set_allocator(evio_realloc_cb cb, void *ctx);

enum {
    EVIO_MASK       = 0xFFFF,
    EVIO_NONE       = 0x0000,
    EVIO_READ       = 0x0001,
    EVIO_WRITE      = 0x0002,
    EVIO_POLL       = 0x0004,
    EVIO_TIMER      = 0x0008,
    EVIO_CRON       = 0x0010,
    EVIO_SIGNAL     = 0x0020,
    EVIO_ASYNC      = 0x0040,
    EVIO_IDLE       = 0x0080,
    EVIO_PREPARE    = 0x0100,
    EVIO_CHECK      = 0x0200,
    EVIO_CLEANUP    = 0x0400,
    EVIO_CUSTOM     = 0x0800,
    EVIO_WALK       = 0x4000,
    EVIO_ERROR      = 0x8000,
};

typedef struct evio_loop    evio_loop;
typedef struct evio_base    evio_base;
typedef struct evio_list    evio_list;
typedef struct evio_poll    evio_poll;
typedef struct evio_timer   evio_timer;
typedef struct evio_cron    evio_cron;
typedef struct evio_signal  evio_signal;
typedef struct evio_async   evio_async;
typedef struct evio_idle    evio_idle;
typedef struct evio_prepare evio_prepare;
typedef struct evio_check   evio_check;
typedef struct evio_cleanup evio_cleanup;

typedef void (*evio_cb)(evio_loop *loop, evio_base *base, uint16_t emask);

__evio_public __evio_nodiscard
evio_loop *evio_loop_new(int maxevents);

__evio_public
void evio_loop_free(evio_loop *loop);

__evio_public __evio_nonnull(1) __evio_nodiscard
evio_time_t evio_loop_time(evio_loop *loop);

__evio_public __evio_nonnull(1) __evio_nodiscard
evio_time_t evio_loop_monotonic_time(evio_loop *loop);

__evio_public __evio_nonnull(1)
void evio_loop_update_time(evio_loop *loop);

__evio_public __evio_nonnull(1)
void evio_loop_ref(evio_loop *loop);

__evio_public __evio_nonnull(1)
void evio_loop_unref(evio_loop *loop);

__evio_public __evio_nonnull(1) __evio_nodiscard
size_t evio_loop_refcount(evio_loop *loop);

__evio_public __evio_nonnull(1)
void *evio_loop_set_data(evio_loop *loop, void *data);

__evio_public __evio_nonnull(1) __evio_nodiscard
void *evio_loop_get_data(evio_loop *loop);

enum {
    EVIO_RUN_DEFAULT    = 0,
    EVIO_RUN_NOWAIT     = 1,
    EVIO_RUN_ONCE       = 2,
};

__evio_public __evio_nonnull(1)
size_t evio_loop_run(evio_loop *loop, uint8_t run_mode);

enum {
    EVIO_BREAK_CANCEL   = 0,
    EVIO_BREAK_ONE      = 1,
    EVIO_BREAK_ALL      = 2,
};

__evio_public __evio_nonnull(1)
void evio_loop_break(evio_loop *loop, uint8_t break_mode);

__evio_public __evio_nonnull(1, 2)
void evio_loop_walk(evio_loop *loop, evio_cb cb, uint16_t emask);

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

__evio_public __evio_nonnull(1, 2)
void evio_feed_event(evio_loop *loop, evio_base *base, uint16_t emask);

__evio_public __evio_nonnull(1)
void evio_feed_fd_event(evio_loop *loop, int fd, uint16_t emask);

__evio_public __evio_nonnull(1)
void evio_feed_fd_close(evio_loop *loop, int fd);

__evio_public
void evio_feed_signal(int signum);

__evio_public __evio_nonnull(1)
void evio_feed_signal_event(evio_loop *loop, int signum);

__evio_public __evio_nonnull(1)
void evio_invoke_pending(evio_loop *loop);

static __evio_inline __evio_nonnull(1, 2)
void evio_base_init(evio_base *base, evio_cb cb)
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

static __evio_inline __evio_nonnull(1)
void evio_poll_modify(evio_poll *w, uint8_t emask)
{
    w->emask = (emask & (EVIO_READ | EVIO_WRITE)) | (w->emask & EVIO_POLL);
}

static __evio_inline __evio_nonnull(1)
void evio_poll_set(evio_poll *w, int fd, uint8_t emask)
{
    w->fd = fd;
    w->emask = (emask & (EVIO_READ | EVIO_WRITE)) | EVIO_POLL;
}

static __evio_inline __evio_nonnull(1, 2)
void evio_poll_init(evio_poll *w, evio_cb cb, int fd, uint8_t emask)
{
    evio_base_init(&w->base, cb);
    evio_poll_set(w, fd, emask);
}

__evio_public __evio_nonnull(1, 2)
void evio_poll_start(evio_loop *loop, evio_poll *w);

__evio_public __evio_nonnull(1, 2)
void evio_poll_stop(evio_loop *loop, evio_poll *w);

// ####################################################################
// EVIO_TIMER
// ####################################################################

struct evio_timer {
    EVIO_BASE;
    evio_time_t at;
    evio_time_t repeat;
};

static __evio_inline __evio_nonnull(1)
void evio_timer_set(evio_timer *w,
                    evio_time_t after, evio_time_t repeat)
{
    w->at = after;
    w->repeat = repeat;
}

static __evio_inline __evio_nonnull(1, 2)
void evio_timer_init(evio_timer *w, evio_cb cb,
                     evio_time_t after, evio_time_t repeat)
{
    evio_base_init(&w->base, cb);
    evio_timer_set(w, after, repeat);
}

__evio_public __evio_nonnull(1, 2)
void evio_timer_start(evio_loop *loop, evio_timer *w);

__evio_public __evio_nonnull(1, 2)
void evio_timer_stop(evio_loop *loop, evio_timer *w);

__evio_public __evio_nonnull(1, 2)
void evio_timer_again(evio_loop *loop, evio_timer *w);

__evio_public __evio_nonnull(1, 2) __evio_nodiscard
evio_time_t evio_timer_remaining(evio_loop *loop, evio_timer *w);

// ####################################################################
// EVIO_CRON
// ####################################################################

struct evio_cron {
    EVIO_BASE;
    evio_time_t at;
    evio_time_t offset;
    evio_time_t interval;
};

static __evio_inline __evio_nonnull(1)
void evio_cron_set(evio_cron *w,
                   evio_time_t offset, evio_time_t interval)
{
    w->offset = offset;
    w->interval = interval;
}

static __evio_inline __evio_nonnull(1, 2)
void evio_cron_init(evio_cron *w, evio_cb cb,
                    evio_time_t offset, evio_time_t interval)
{
    evio_base_init(&w->base, cb);
    evio_cron_set(w, offset, interval);
    w->at = 0;
}

__evio_public __evio_nonnull(1, 2)
void evio_cron_start(evio_loop *loop, evio_cron *w);

__evio_public __evio_nonnull(1, 2)
void evio_cron_stop(evio_loop *loop, evio_cron *w);

__evio_public __evio_nonnull(1, 2)
void evio_cron_again(evio_loop *loop, evio_cron *w);

// ####################################################################
// EVIO_SIGNAL
// ####################################################################

struct evio_signal {
    EVIO_LIST;
    int signum;
};

static __evio_inline __evio_nonnull(1)
void evio_signal_set(evio_signal *w, int signum)
{
    w->signum = signum;
}

static __evio_inline __evio_nonnull(1, 2)
void evio_signal_init(evio_signal *w, evio_cb cb, int signum)
{
    evio_base_init(&w->base, cb);
    evio_signal_set(w, signum);
}

__evio_public __evio_nonnull(1, 2)
void evio_signal_start(evio_loop *loop, evio_signal *w);

__evio_public __evio_nonnull(1, 2)
void evio_signal_stop(evio_loop *loop, evio_signal *w);

// ####################################################################
// EVIO_ASYNC
// ####################################################################

struct evio_async {
    EVIO_BASE;
    _Atomic uint8_t status;
};

static __evio_inline __evio_nonnull(1, 2)
void evio_async_init(evio_async *w, evio_cb cb)
{
    evio_base_init(&w->base, cb);
    w->status = 0;
}

__evio_public __evio_nonnull(1, 2)
void evio_async_start(evio_loop *loop, evio_async *w);

__evio_public __evio_nonnull(1, 2)
void evio_async_stop(evio_loop *loop, evio_async *w);

__evio_public __evio_nonnull(1, 2)
void evio_async_send(evio_loop *loop, evio_async *w);

// ####################################################################
// EVIO_IDLE
// ####################################################################

struct evio_idle {
    EVIO_BASE;
};

static __evio_inline __evio_nonnull(1, 2)
void evio_idle_init(evio_idle *w, evio_cb cb)
{
    evio_base_init(&w->base, cb);
}

__evio_public __evio_nonnull(1, 2)
void evio_idle_start(evio_loop *loop, evio_idle *w);

__evio_public __evio_nonnull(1, 2)
void evio_idle_stop(evio_loop *loop, evio_idle *w);

// ####################################################################
// EVIO_PREPARE
// ####################################################################

struct evio_prepare {
    EVIO_BASE;
};

static __evio_inline __evio_nonnull(1, 2)
void evio_prepare_init(evio_prepare *w, evio_cb cb)
{
    evio_base_init(&w->base, cb);
}

__evio_public __evio_nonnull(1, 2)
void evio_prepare_start(evio_loop *loop, evio_prepare *w);

__evio_public __evio_nonnull(1, 2)
void evio_prepare_stop(evio_loop *loop, evio_prepare *w);

// ####################################################################
// EVIO_CHECK
// ####################################################################

struct evio_check {
    EVIO_BASE;
};

static __evio_inline __evio_nonnull(1, 2)
void evio_check_init(evio_check *w, evio_cb cb)
{
    evio_base_init(&w->base, cb);
}

__evio_public __evio_nonnull(1, 2)
void evio_check_start(evio_loop *loop, evio_check *w);

__evio_public __evio_nonnull(1, 2)
void evio_check_stop(evio_loop *loop, evio_check *w);

// ####################################################################
// EVIO_CLEANUP
// ####################################################################

struct evio_cleanup {
    EVIO_BASE;
};

static __evio_inline __evio_nonnull(1, 2)
void evio_cleanup_init(evio_cleanup *w, evio_cb cb)
{
    evio_base_init(&w->base, cb);
}

__evio_public __evio_nonnull(1, 2)
void evio_cleanup_start(evio_loop *loop, evio_cleanup *w);

__evio_public __evio_nonnull(1, 2)
void evio_cleanup_stop(evio_loop *loop, evio_cleanup *w);

// ####################################################################

typedef union evio_watcher {
    evio_base base;
    evio_list list;
    evio_poll poll;
    evio_timer timer;
    evio_cron cron;
    evio_signal signal;
    evio_async async;
    evio_idle idle;
    evio_prepare prepare;
    evio_check check;
    evio_cleanup cleanup;
} evio_watcher;

#ifdef __cplusplus
}
#endif
