#include "evio_core.h"
#include "evio_utils.h"

#include <errno.h>

static struct {
    evio_abort_cb cb;
    void *ctx;
} evio_abort_custom = {
    .cb = NULL,
    .ctx = NULL,
};

void evio_set_abort(evio_abort_cb cb, void *ctx)
{
    if (cb) {
        evio_abort_custom.cb = cb;
        evio_abort_custom.ctx = ctx;
    } else {
        evio_abort_custom.cb = NULL;
        evio_abort_custom.ctx = NULL;
    }
}

evio_abort_cb evio_get_abort(void **ctx)
{
    if (ctx) {
        *ctx = evio_abort_custom.ctx;
    }
    return evio_abort_custom.cb;
}

static void (*evio_default_abort)(void) = abort;

__evio_nonnull(1, 4) __evio_format_printf(4, 0)
static size_t evio_buf_append_vprintf(char *buf, size_t size, size_t len,
                                      const char *restrict format, va_list ap)
{
    EVIO_ASSERT(size);
    EVIO_ASSERT(len < size);

    size_t avail = size - len;
    int ret = vsnprintf(buf + len, avail, format, ap);
    if (ret < 0) {
        return len;
    }

    size_t n = (size_t)ret;
    if (n >= avail) {
        return size - 1;
    }
    return len + n;
}

__evio_nonnull(1, 4) __evio_format_printf(4, 5)
static size_t evio_buf_append_printf(char *buf, size_t size, size_t len,
                                     const char *restrict format, ...)
{
    va_list ap;
    va_start(ap, format);
    len = evio_buf_append_vprintf(buf, size, len, format, ap);
    va_end(ap);
    return len;
}

void evio_set_abort_func(void (*func)(void))
{
    evio_default_abort = func ? func : abort;
}

void (*evio_get_abort_func(void))(void)
{
    return evio_default_abort;
}

void evio_abort(const char *restrict file, int line,
                const char *restrict func, const char *restrict format, ...)
{
    FILE *stream = evio_abort_custom.cb ?
                   evio_abort_custom.cb(evio_abort_custom.ctx) : stderr;

    if (stream) {
        char str[4096];
        size_t len = 0;

        len = evio_buf_append_printf(str, sizeof(str), len,
                                     "\nABORT in %s(): %s:%d\n\n", func, file, line);

        if (*format) {
            va_list ap;
            va_start(ap, format);
            len = evio_buf_append_vprintf(str, sizeof(str), len, format, ap);
            va_end(ap);

            len = evio_buf_append_printf(str, sizeof(str), len, "\n");
        }

        fwrite(str, 1, len, stream);
        fflush(stream);
    }

    evio_default_abort();

    __builtin_unreachable(); // GCOVR_EXCL_LINE
}

#ifdef __GLIBC__

char *evio_strerror(int err, char *data, size_t size)
{
    const char *desc = strerrordesc_np(err);
    if (__evio_unlikely(!desc)) {
        snprintf(data, size, "Unknown error %d", err);
    } else {
        snprintf(data, size, "%s (%s)", desc, strerrorname_np(err));
    }
    return data;
}

#else // __GLIBC__

char *evio_strerror(int err, char *data, size_t size)
{
    int rc = strerror_r(err, data, size);
    if (__evio_unlikely(rc != 0)) {
        int e = rc == -1 ? errno : rc;
        if (e == ERANGE) {
            return data;
        }
        snprintf(data, size, "Unknown error %d", err);
    }
    return data;
}

#endif
