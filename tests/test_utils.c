#include "test.h"

static jmp_buf abort_jmp_buf;
static size_t custom_abort_called;

static FILE *custom_abort_handler(void *ctx)
{
    custom_abort_called++;
    longjmp(abort_jmp_buf, 1);
    return NULL; // GCOVR_EXCL_LINE
}

static jmp_buf test_abort_jmp_buf;

static void test_abort_func(void)
{
    longjmp(test_abort_jmp_buf, 1);
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

    // Test with macro
    const char *macro_err_str = EVIO_STRERROR(EPERM);
    assert_string_not_equal(macro_err_str, "");
}

TEST(test_evio_strerror_invalid)
{
    char buf[EVIO_STRERROR_SIZE];
    // Use a large number that is likely not a valid errno
    char *err_str = evio_strerror(99999, buf, sizeof(buf));
    assert_ptr_equal(err_str, buf);

#ifdef __GLIBC__
    assert_string_equal(err_str, "Unknown error 99999");
#else
    assert_string_equal(err_str, "No error information");
#endif
}

TEST(test_evio_strerror_erange)
{
    char buf[18];
    // Use a large number that is likely not a valid errno
    char *err_str = evio_strerror(99999, buf, sizeof(buf));
    assert_ptr_equal(err_str, buf);

    // snprintf will truncate the string to fit in the buffer.
    // "Unknown error 99999" is 19 chars + null.
    // With a buffer of 18, it should be truncated to 17 chars + null.
    assert_string_equal(err_str, "Unknown error 999");
}

TEST(test_evio_utils_abort_custom)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);
    evio_set_abort(custom_abort_handler, NULL);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        EVIO_ABORT("Testing custom abort\n");
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_utils_abort_no_format)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);
    evio_set_abort(custom_abort_handler, NULL);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        EVIO_ABORT(""); // empty format string
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_get_abort_null_ctx)
{
    // Save original
    void *old_ctx;
    evio_abort_cb old_cb = evio_get_abort(&old_ctx);
    void *my_ctx = (void *)0xdeadbeef;
    evio_set_abort(custom_abort_handler, my_ctx);

    // Call with NULL ctx, should not crash and should return the cb.
    // This covers the if(ctx) == false branch in evio_get_abort.
    evio_abort_cb cb = evio_get_abort(NULL);
    assert_ptr_equal(cb, custom_abort_handler);

    // Restore original
    evio_set_abort(old_cb, old_ctx);
}

TEST(test_evio_abort_default_handler)
{
    // Save original handlers
    void *old_abort_ctx;
    evio_abort_cb old_abort_cb = evio_get_abort(&old_abort_ctx);
    void (*old_abort_func)(void) = evio_get_abort_func();
    evio_set_abort_func(test_abort_func);

    // Set custom abort handler to NULL to test the default path
    evio_set_abort(NULL, NULL);

    if (setjmp(test_abort_jmp_buf) == 0) {
        EVIO_ABORT("Testing default abort\n");
        fail(); // GCOVR_EXCL_LINE
    }
    // if we reach here, longjmp worked.

    // Restore original state
    evio_set_abort_func(old_abort_func);
    evio_set_abort(old_abort_cb, old_abort_ctx);
}

TEST(test_evio_get_set_abort_func)
{
    void (*old_abort_func)(void) = evio_get_abort_func();
    assert_ptr_equal(old_abort_func, abort);

    evio_set_abort_func(test_abort_func);
    assert_ptr_equal(evio_get_abort_func(), test_abort_func);

    evio_set_abort_func(NULL);
    assert_ptr_equal(evio_get_abort_func(), abort);

    // Restore original in case it wasn't abort.
    evio_set_abort_func(old_abort_func);
}

static FILE *null_stream_abort_handler(void *ctx)
{
    custom_abort_called++;
    return NULL;
}

TEST(test_evio_abort_no_stream)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort_cb = evio_get_abort(&old_abort_ctx);

    void (*old_abort_func)(void) = evio_get_abort_func();
    evio_set_abort_func(test_abort_func);

    evio_set_abort(null_stream_abort_handler, NULL);
    custom_abort_called = 0;

    if (setjmp(test_abort_jmp_buf) == 0) {
        EVIO_ABORT("This message should not be printed");
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_set_abort_func(old_abort_func);
    evio_set_abort(old_abort_cb, old_abort_ctx);
}

TEST(test_evio_abort_default_handler_empty_format)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort_cb = evio_get_abort(&old_abort_ctx);

    void (*old_abort_func)(void) = evio_get_abort_func();
    evio_set_abort_func(test_abort_func);

    evio_set_abort(NULL, NULL);

    if (setjmp(test_abort_jmp_buf) == 0) {
        EVIO_ABORT("");
        fail(); // GCOVR_EXCL_LINE
    }

    evio_set_abort_func(old_abort_func);
    evio_set_abort(old_abort_cb, old_abort_ctx);
}
