#include "evio_core.h"
#include "evio_utils.h"

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
        char *p = str;

        p += snprintf(p, sizeof(str) - (size_t)(p - str),
                      "\nABORT in %s(): %s:%d\n\n", func, file, line);

        if (*format) {
            va_list ap;
            va_start(ap, format);
            p += vsnprintf(p, sizeof(str) - (size_t)(p - str), format, ap);
            va_end(ap);

            p += snprintf(p, sizeof(str) - (size_t)(p - str), "\n");
        }

        fwrite(str, 1, (size_t)(p - str), stream);
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
    if (__evio_unlikely(strerror_r(err, data, size) != 0)) {
        snprintf(data, size, "Unknown error %d", err);
    }
    return data;
}

#endif
