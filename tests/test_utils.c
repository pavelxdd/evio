#include "test.h"
#include "abort.h"

#include <wchar.h>

static FILE *custom_abort_handler(void *ctx)
{
    volatile size_t *count = ctx;
    (*count)++;
    return NULL;
}

TEST(test_evio_strerror_valid)
{
    char buf[EVIO_STRERROR_SIZE];
    char *err_str = evio_strerror(EAGAIN, buf, sizeof(buf));
    assert_ptr_equal(err_str, buf);
    assert_string_not_equal(err_str, "");

    err_str = evio_strerror(EINVAL, buf, sizeof(buf));
    assert_ptr_equal(err_str, buf);
    assert_string_not_equal(err_str, "");

    const char *macro_err_str = EVIO_STRERROR(EPERM);
    assert_string_not_equal(macro_err_str, "");
}

TEST(test_evio_strerror_invalid)
{
    char buf[EVIO_STRERROR_SIZE];
    char *err_str = evio_strerror(99999, buf, sizeof(buf));
    assert_ptr_equal(err_str, buf);

#ifdef __GLIBC__
    assert_string_equal(err_str, "Unknown error 99999");
#else
    assert_string_equal(err_str, "No error information");
#endif
}

TEST(test_evio_strerror_negative)
{
    char buf[EVIO_STRERROR_SIZE];
    char *err_str = evio_strerror(-1, buf, sizeof(buf));
    assert_ptr_equal(err_str, buf);
    assert_string_not_equal(err_str, "");
}

TEST(test_evio_strerror_erange)
{
    char buf[18];
    char *err_str = evio_strerror(99999, buf, sizeof(buf));
    assert_ptr_equal(err_str, buf);

#ifdef __GLIBC__
    assert_string_equal(err_str, "Unknown error 999");
#else
    assert_string_equal(err_str, "No error informat");
#endif
}

TEST(test_evio_utils_abort_custom)
{
    jmp_buf jmp;
    struct evio_test_abort_state st;
    volatile size_t custom_abort_called = 0;

    evio_test_abort_begin(&st, &jmp);
    evio_set_abort(custom_abort_handler, (void *)&custom_abort_called);

    if (setjmp(jmp) == 0) {
        EVIO_ABORT("Testing custom abort\n");
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_test_abort_end(&st);
}

TEST(test_evio_utils_abort_no_format)
{
    jmp_buf jmp;
    struct evio_test_abort_state st;
    volatile size_t custom_abort_called = 0;

    evio_test_abort_begin(&st, &jmp);
    evio_set_abort(custom_abort_handler, (void *)&custom_abort_called);

    if (setjmp(jmp) == 0) {
        EVIO_ABORT(""); // empty format string
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_test_abort_end(&st);
}

TEST(test_evio_get_abort_null_ctx)
{
    void *old_ctx;
    evio_abort_cb old_cb = evio_get_abort(&old_ctx);
    void *my_ctx = (void *)0xdeadbeef;
    evio_set_abort(custom_abort_handler, my_ctx);

    evio_abort_cb cb = evio_get_abort(NULL);
    assert_ptr_equal(cb, custom_abort_handler);

    evio_set_abort(old_cb, old_ctx);
}

TEST(test_evio_abort_default_handler)
{
    jmp_buf jmp;
    struct evio_test_abort_state st;

    evio_test_abort_begin(&st, &jmp);
    evio_set_abort(NULL, NULL);

    if (setjmp(jmp) == 0) {
        EVIO_ABORT("Testing default abort\n");
        fail(); // GCOVR_EXCL_LINE
    }
    evio_test_abort_end(&st);
}

static size_t test_abort_called;

static void test_abort_func(void)
{
    test_abort_called++;
}

TEST(test_evio_get_set_abort_func)
{
    void (*old_abort_func)(void) = evio_get_abort_func();
    assert_ptr_equal(old_abort_func, abort);

    test_abort_called = 0;
    evio_set_abort_func(test_abort_func);
    assert_ptr_equal(evio_get_abort_func(), test_abort_func);
    evio_get_abort_func()();
    assert_int_equal(test_abort_called, 1);

    evio_set_abort_func(NULL);
    assert_ptr_equal(evio_get_abort_func(), abort);

    // Restore original in case it wasn't abort.
    evio_set_abort_func(old_abort_func);
}

static FILE *null_stream_abort_handler(void *ctx)
{
    return NULL;
}

TEST(test_evio_abort_no_stream)
{
    jmp_buf jmp;
    struct evio_test_abort_state st;

    evio_test_abort_begin(&st, &jmp);
    evio_set_abort(null_stream_abort_handler, NULL);

    if (setjmp(jmp) == 0) {
        EVIO_ABORT("This message should not be printed");
        fail(); // GCOVR_EXCL_LINE
    }
    evio_test_abort_end(&st);
}

TEST(test_evio_abort_test_handler)
{
    jmp_buf jmp;
    struct evio_test_abort_state st;

    evio_test_abort_begin(&st, &jmp);

    if (setjmp(jmp) == 0) {
        EVIO_ABORT("x");
        fail(); // GCOVR_EXCL_LINE
    }

    evio_test_abort_end(&st);
}

TEST(test_evio_abort_default_handler_empty_format)
{
    jmp_buf jmp;
    struct evio_test_abort_state st;

    evio_test_abort_begin(&st, &jmp);
    evio_set_abort(NULL, NULL);

    if (setjmp(jmp) == 0) {
        EVIO_ABORT("");
        fail(); // GCOVR_EXCL_LINE
    }
    evio_test_abort_end(&st);
}

TEST(test_evio_abort_long_message)
{
    jmp_buf jmp;
    struct evio_test_abort_state st;
    char msg[8192];

    memset(msg, 'A', sizeof(msg));
    msg[sizeof(msg) - 1] = '\0';

    evio_test_abort_begin(&st, &jmp);
    evio_set_abort(NULL, NULL);

    if (setjmp(jmp) == 0) {
        EVIO_ABORT("%s", msg);
        fail(); // GCOVR_EXCL_LINE
    }

    evio_test_abort_end(&st);
}

TEST(test_evio_abort_vsnprintf_error)
{
    jmp_buf jmp;
    struct evio_test_abort_state st;

    wchar_t bad[2] = { 0x110000, 0 };

    evio_test_abort_begin(&st, &jmp);
    evio_set_abort(NULL, NULL);

    if (setjmp(jmp) == 0) {
        EVIO_ABORT("%ls", bad);
        fail(); // GCOVR_EXCL_LINE
    }

    evio_test_abort_end(&st);
}
