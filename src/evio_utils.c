#include "evio_core.h"
#include "evio_utils.h"

static evio_abort_cb evio_abort_custom = NULL;

void evio_set_abort(evio_abort_cb cb)
{
    evio_abort_custom = cb;
}

evio_abort_cb evio_get_abort(void)
{
    return evio_abort_custom;
}

void evio_abort(const char *restrict file, int line,
                const char *restrict func, const char *restrict format, ...)
{
    FILE *stream = stderr;

    if (evio_abort_custom) {
        va_list ap;
        va_start(ap, format);
        stream = evio_abort_custom(file, line, func, format, ap);
        va_end(ap);
    }

    // GCOVR_EXCL_START
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
    // GCOVR_EXCL_STOP

    abort();
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
