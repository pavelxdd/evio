#include "test.h"

static jmp_buf abort_jmp_buf;
static size_t custom_abort_called;

static FILE *custom_abort_handler(void *ctx)
{
    custom_abort_called++;
    longjmp(abort_jmp_buf, 1);
    return NULL; // GCOVR_EXCL_LINE
}

TEST(test_evio_malloc)
{
    for (size_t i = 1; i < 100; ++i) {
        void *ptr = evio_malloc(i);
        assert_non_null(ptr);
        evio_free(ptr);
    }
}

TEST(test_evio_calloc)
{
    for (size_t i = 1; i < 10; ++i) {
        for (size_t j = 1; j < 10; ++j) {
            char *ptr = evio_calloc(i, j);
            assert_non_null(ptr);

            for (size_t k = 0; k < i * j; ++k) {
                assert_int_equal(ptr[k], 0);
            }

            evio_free(ptr);
        }
    }
}

TEST(test_evio_realloc)
{
    void *ptr = evio_realloc(NULL, 1);
    assert_non_null(ptr);

    ptr = evio_realloc(ptr, 100);
    assert_non_null(ptr);

    ptr = evio_realloc(ptr, 1);
    assert_non_null(ptr);

    evio_free(ptr);
}

TEST(test_evio_reallocarray)
{
    void *ptr = evio_reallocarray(NULL, 1, 1);
    assert_non_null(ptr);

    ptr = evio_reallocarray(ptr, 10, 10);
    assert_non_null(ptr);

    ptr = evio_reallocarray(ptr, 1, 1);
    assert_non_null(ptr);

    evio_free(ptr);
}

static size_t custom_alloc_count = 0;

static void *custom_realloc(void *ctx, void *ptr, size_t size)
{
    custom_alloc_count++;

    if (size) {
        return realloc(ptr, size);
    }

    free(ptr);
    return NULL;
}

TEST(test_evio_custom_allocator)
{
    // Save original allocator
    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    custom_alloc_count = 0;
    evio_set_allocator(custom_realloc, NULL);

    evio_realloc_cb allocator = evio_get_allocator(NULL);
    assert_ptr_equal(allocator, custom_realloc);

    void *ptr = evio_malloc(10);
    assert_non_null(ptr);
    assert_int_equal(custom_alloc_count, 1);

    ptr = evio_realloc(ptr, 20);
    assert_non_null(ptr);
    assert_int_equal(custom_alloc_count, 2);

    evio_free(ptr);
    assert_int_equal(custom_alloc_count, 3);

    // Restore original allocator
    evio_set_allocator(old_alloc, old_alloc_ctx);

    ptr = evio_malloc(10);
    assert_non_null(ptr);

    evio_free(ptr);
    assert_int_equal(custom_alloc_count, 3); // should not have increased
}

TEST(test_evio_set_allocator_null)
{
    // Save original allocator
    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    // Set NULL cb, should default to evio_default_realloc
    evio_set_allocator(NULL, (void *)0xdeadbeef);

    void *ctx;
    evio_realloc_cb allocator = evio_get_allocator(&ctx);
    assert_ptr_not_equal(allocator, NULL);
    assert_ptr_equal(ctx, (void *)0xdeadbeef);

    // check it works
    void *ptr = evio_malloc(10);
    assert_non_null(ptr);
    evio_free(ptr);

    // Restore original allocator
    evio_set_allocator(old_alloc, old_alloc_ctx);
}

TEST(test_evio_malloc_overflow)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);
    evio_set_abort(custom_abort_handler, NULL);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        evio_malloc(PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(custom_abort_called, 1);
    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_calloc_overflow_size)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);
    evio_set_abort(custom_abort_handler, NULL);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        evio_calloc(1, PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(custom_abort_called, 1);
    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_calloc_overflow_mul)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);
    evio_set_abort(custom_abort_handler, NULL);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        evio_calloc(PTRDIFF_MAX, PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(custom_abort_called, 1);
    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_realloc_overflow)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);
    evio_set_abort(custom_abort_handler, NULL);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        evio_realloc(NULL, PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(custom_abort_called, 1);
    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_reallocarray_overflow_size)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);
    evio_set_abort(custom_abort_handler, NULL);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        evio_reallocarray(NULL, 1, PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(custom_abort_called, 1);
    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_reallocarray_overflow_mul)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);
    evio_set_abort(custom_abort_handler, NULL);
    custom_abort_called = 0;

    if (setjmp(abort_jmp_buf) == 0) {
        evio_reallocarray(NULL, PTRDIFF_MAX, PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(custom_abort_called, 1);
    evio_set_abort(old_abort, old_abort_ctx);
}

static void *failing_allocator(void *ctx, void *ptr, size_t size)
{
    return NULL;
}

TEST(test_evio_malloc_failing)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);

    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    evio_set_abort(custom_abort_handler, NULL);
    evio_set_allocator(failing_allocator, NULL);

    custom_abort_called = 0;
    if (setjmp(abort_jmp_buf) == 0) {
        evio_malloc(1);
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_set_allocator(old_alloc, old_alloc_ctx);
    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_calloc_failing)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);

    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    evio_set_abort(custom_abort_handler, NULL);
    evio_set_allocator(failing_allocator, NULL);

    custom_abort_called = 0;
    if (setjmp(abort_jmp_buf) == 0) {
        evio_calloc(1, 1);
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_set_allocator(old_alloc, old_alloc_ctx);
    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_realloc_failing)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);

    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    evio_set_abort(custom_abort_handler, NULL);
    evio_set_allocator(failing_allocator, NULL);

    custom_abort_called = 0;
    if (setjmp(abort_jmp_buf) == 0) {
        evio_realloc(NULL, 1);
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_set_allocator(old_alloc, old_alloc_ctx);
    evio_set_abort(old_abort, old_abort_ctx);
}

TEST(test_evio_reallocarray_failing)
{
    void *old_abort_ctx;
    evio_abort_cb old_abort = evio_get_abort(&old_abort_ctx);

    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    evio_set_abort(custom_abort_handler, NULL);
    evio_set_allocator(failing_allocator, NULL);

    custom_abort_called = 0;
    if (setjmp(abort_jmp_buf) == 0) {
        evio_reallocarray(NULL, 1, 1);
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(custom_abort_called, 1);

    evio_set_allocator(old_alloc, old_alloc_ctx);
    evio_set_abort(old_abort, old_abort_ctx);
}
