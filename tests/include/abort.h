#pragma once

#include <setjmp.h>
#include <unistd.h>

#include "evio.h"

struct evio_test_abort_state {
    evio_abort_cb old_abort_cb;
    void *old_abort_ctx;
    void (*old_abort_func)(void);
    FILE *stream;
    bool stream_owned;
    int stderr_fd;
    bool stderr_redirected;
};

struct evio_test_abort_ctx {
    struct evio_test_abort_state st;
    volatile size_t called;
    evio_abort_cb cb;
    void *cb_ctx;
};

static jmp_buf *evio_test_abort_jmp;

static void evio_test_abort_func(void)
{
    longjmp(*evio_test_abort_jmp, 1);
}

static FILE *evio_test_abort_ctx_handler(void *ctx)
{
    struct evio_test_abort_ctx *t = ctx;
    t->called++;

    if (t->cb) {
        FILE *stream = t->cb(t->cb_ctx);
        if (stream) {
            return stream;
        }
    }

    return t->st.stream;
}

static FILE *evio_test_abort_handler(void *ctx)
{
    struct evio_test_abort_state *st = ctx;
    return st->stream;
}

static inline void evio_test_abort_begin(struct evio_test_abort_state *st, jmp_buf *jmp)
{
    st->old_abort_cb = evio_get_abort(&st->old_abort_ctx);
    st->old_abort_func = evio_get_abort_func();

    st->stream = tmpfile();
    st->stream_owned = st->stream != NULL;
    if (!st->stream) {
        st->stream = fopen("/dev/null", "w");
        st->stream_owned = st->stream != NULL;
        if (!st->stream) {
            st->stream = stderr;
        }
    }

    st->stderr_fd = dup(fileno(stderr));
    st->stderr_redirected = st->stderr_fd >= 0 &&
                            dup2(fileno(st->stream), fileno(stderr)) >= 0;

    evio_test_abort_jmp = jmp;
    evio_set_abort_func(evio_test_abort_func);
    evio_set_abort(evio_test_abort_handler, st);
}

static inline void evio_test_abort_end(struct evio_test_abort_state *st)
{
    evio_set_abort_func(st->old_abort_func);
    evio_set_abort(st->old_abort_cb, st->old_abort_ctx);

    if (st->stderr_redirected) {
        (void)dup2(st->stderr_fd, fileno(stderr));
    }
    if (st->stderr_fd >= 0) {
        close(st->stderr_fd);
    }

    if (st->stream_owned) {
        fclose(st->stream);
    }

    evio_test_abort_jmp = NULL;
}

static inline void evio_test_abort_ctx_begin(struct evio_test_abort_ctx *ctx, jmp_buf *jmp)
{
    ctx->called = 0;
    ctx->cb = NULL;
    ctx->cb_ctx = NULL;

    evio_test_abort_begin(&ctx->st, jmp);
    evio_set_abort(evio_test_abort_ctx_handler, ctx);
}

static inline void evio_test_abort_ctx_end(struct evio_test_abort_ctx *ctx)
{
    evio_test_abort_end(&ctx->st);
}
