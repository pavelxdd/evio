#pragma once

// IWYU pragma: begin_exports

#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <setjmp.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "evio_core.h"

#include <cmocka.h>

// IWYU pragma: end_exports

#ifndef __evio_used
#   if __evio_has_attribute(__unused__)
#       define __evio_used __attribute__((__used__))
#   else
#       define __evio_used
#   endif
#endif

#ifndef __evio_unused
#   if __evio_has_attribute(__unused__)
#       define __evio_unused __attribute__((__unused__))
#   else
#       define __evio_unused
#   endif
#endif

#ifndef __evio_aligned
#   if __evio_has_attribute(__aligned__)
#       define __evio_aligned(x) __attribute__((__aligned__(x)))
#   else
#       error "attribute aligned is not supported"
#   endif
#endif

#ifndef __evio_section
#   if __evio_has_attribute(__section__)
#       define __evio_section(x) __attribute__((__section__(x)))
#   else
#       error "attribute section is not supported"
#   endif
#endif

#ifndef __STRING
#define __STRING(x) #x
#endif
#define STRING(x) __STRING(x)

#ifndef __CONCAT
#define __CONCAT(x, y) x ## y
#endif
#define CONCAT(x, y) __CONCAT(x, y)

#define TEST_NAME(__name) \
    (__name "\0" __FILE__ ":" STRING(__LINE__))

#define TEST_FUNC(__name, __func, ...) \
    __evio_section("unit_test_section") __evio_used __evio_aligned(sizeof(long)) \
    static const struct CMUnitTest CONCAT(__func, CONCAT(__LINE_, __COUNTER__)) \
        = { .name = TEST_NAME(__name), .test_func = __func, ##__VA_ARGS__ }

#define TEST_UNIT(__name, __func, ...) \
    static void __func (void **state __evio_unused); \
    TEST_FUNC(__name, __func, ##__VA_ARGS__); \
    static void __func (void **state __evio_unused)

#define TEST(__func, ...) TEST_UNIT(#__func, __func, ##__VA_ARGS__)

extern size_t generic_cb_called;
extern evio_mask generic_cb_emask;
extern size_t generic_cb2_called;
extern size_t break_cb_called;

void generic_cb(evio_loop *loop, evio_base *w, evio_mask emask);
void generic_cb2(evio_loop *loop, evio_base *w, evio_mask emask);
void break_cb(evio_loop *loop, evio_base *w, evio_mask emask);
void reset_cb_state(void);
void read_and_count_cb(evio_loop *loop, evio_base *w, evio_mask emask);

extern jmp_buf abort_jmp_buf;
extern size_t custom_abort_called;

FILE *custom_abort_handler(void *ctx);

extern jmp_buf test_abort_jmp_buf;
void test_abort_func(void);
