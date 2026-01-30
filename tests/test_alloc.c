#include "test.h"
#include "abort.h"

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

TEST(test_evio_malloc_zero_asserts)
{
    expect_assert_failure(evio_malloc(0));
}

TEST(test_evio_calloc_zero_asserts)
{
    expect_assert_failure(evio_calloc(0, 1));
    expect_assert_failure(evio_calloc(1, 0));
}

TEST(test_evio_realloc_zero_asserts)
{
    expect_assert_failure(evio_realloc(NULL, 0));
}

TEST(test_evio_reallocarray_zero_asserts)
{
    expect_assert_failure(evio_reallocarray(NULL, 0, 1));
    expect_assert_failure(evio_reallocarray(NULL, 1, 0));
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

static void *null_realloc(void *ctx, void *ptr, size_t size)
{
    (void)ctx;

    if (!size) {
        free(ptr);
    }
    return NULL;
}

TEST(test_evio_set_allocator_free)
{
    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    evio_set_allocator(null_realloc, NULL);

    void *ptr = malloc(1);
    assert_non_null(ptr);
    evio_free(ptr);

    evio_set_allocator(old_alloc, old_alloc_ctx);
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
    assert_int_equal(custom_alloc_count, 3);
}

TEST(test_evio_set_allocator_null)
{
    // Save original allocator
    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    // Set NULL cb: default to evio_default_realloc.
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
    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };

    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);
    evio_set_allocator(null_realloc, NULL);

    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    if (setjmp(jmp) == 0) {
        evio_malloc(PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(abort_ctx.called, 1);
    evio_test_abort_ctx_end(&abort_ctx);
    evio_set_allocator(old_alloc, old_alloc_ctx);
}

TEST(test_evio_calloc_overflow_size)
{
    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };

    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);
    evio_set_allocator(null_realloc, NULL);

    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    if (setjmp(jmp) == 0) {
        evio_calloc(1, PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(abort_ctx.called, 1);
    evio_test_abort_ctx_end(&abort_ctx);
    evio_set_allocator(old_alloc, old_alloc_ctx);
}

TEST(test_evio_calloc_overflow_mul)
{
    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };

    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    if (setjmp(jmp) == 0) {
        evio_calloc(PTRDIFF_MAX, PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(abort_ctx.called, 1);
    evio_test_abort_ctx_end(&abort_ctx);
}

TEST(test_evio_realloc_overflow)
{
    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };

    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);
    evio_set_allocator(null_realloc, NULL);

    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    if (setjmp(jmp) == 0) {
        evio_realloc(NULL, PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(abort_ctx.called, 1);
    evio_test_abort_ctx_end(&abort_ctx);
    evio_set_allocator(old_alloc, old_alloc_ctx);
}

TEST(test_evio_reallocarray_overflow_size)
{
    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };

    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);
    evio_set_allocator(null_realloc, NULL);

    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    if (setjmp(jmp) == 0) {
        evio_reallocarray(NULL, 1, PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(abort_ctx.called, 1);
    evio_test_abort_ctx_end(&abort_ctx);
    evio_set_allocator(old_alloc, old_alloc_ctx);
}

TEST(test_evio_reallocarray_overflow_mul)
{
    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };

    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    if (setjmp(jmp) == 0) {
        evio_reallocarray(NULL, PTRDIFF_MAX, PTRDIFF_MAX);
        fail(); // GCOVR_EXCL_LINE
    }

    assert_int_equal(abort_ctx.called, 1);
    evio_test_abort_ctx_end(&abort_ctx);
}

static void *failing_allocator(void *ctx, void *ptr, size_t size)
{
    return NULL;
}

TEST(test_evio_malloc_failing)
{
    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    evio_set_allocator(failing_allocator, NULL);

    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };
    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    if (setjmp(jmp) == 0) {
        evio_malloc(1);
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(abort_ctx.called, 1);
    evio_test_abort_ctx_end(&abort_ctx);

    evio_set_allocator(old_alloc, old_alloc_ctx);
}

TEST(test_evio_calloc_failing)
{
    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    evio_set_allocator(failing_allocator, NULL);

    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };
    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    if (setjmp(jmp) == 0) {
        evio_calloc(1, 1);
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(abort_ctx.called, 1);
    evio_test_abort_ctx_end(&abort_ctx);

    evio_set_allocator(old_alloc, old_alloc_ctx);
}

TEST(test_evio_realloc_failing)
{
    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    evio_set_allocator(failing_allocator, NULL);

    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };
    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    if (setjmp(jmp) == 0) {
        evio_realloc(NULL, 1);
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(abort_ctx.called, 1);
    evio_test_abort_ctx_end(&abort_ctx);

    evio_set_allocator(old_alloc, old_alloc_ctx);
}

TEST(test_evio_reallocarray_failing)
{
    void *old_alloc_ctx;
    evio_realloc_cb old_alloc = evio_get_allocator(&old_alloc_ctx);

    evio_set_allocator(failing_allocator, NULL);

    jmp_buf jmp;
    struct evio_test_abort_ctx abort_ctx = { 0 };
    evio_test_abort_ctx_begin(&abort_ctx, &jmp);

    if (setjmp(jmp) == 0) {
        evio_reallocarray(NULL, 1, 1);
        fail(); // GCOVR_EXCL_LINE
    }
    assert_int_equal(abort_ctx.called, 1);
    evio_test_abort_ctx_end(&abort_ctx);

    evio_set_allocator(old_alloc, old_alloc_ctx);
}
