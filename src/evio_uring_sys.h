#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <linux/io_uring.h>

/** @brief The number of entries for the io_uring submission/completion queues. */
#define EVIO_URING_EVENTS 256

#ifdef EVIO_TESTING

void evio_uring_test_inject_cqe_res_once(int fd, int op, int res);
void evio_uring_test_inject_reset(void);

void evio_uring_test_probe_reset(void);
void evio_uring_test_probe_disable_single_mmap(bool disable);
void evio_uring_test_probe_fail_mmap_at(unsigned call, int err);
void evio_uring_test_probe_fail_setup_once(int err);
void evio_uring_test_probe_fail_register_once(int err);
void evio_uring_test_probe_fail_epoll_create_once(int err);
void evio_uring_test_probe_fail_eventfd_once(int err);
void evio_uring_test_probe_force_sq_off_array_zero(bool force);
void evio_uring_test_probe_enter_ret_once(int ret, int err);
void evio_uring_test_probe_force_cq_empty(bool force);
void evio_uring_test_probe_disable_register_probe(bool disable);
void evio_uring_test_probe_force_cqe_res_once(int res);

int evio_test_uring_setup(unsigned int entries, struct io_uring_params *params);
int evio_test_uring_enter(unsigned int fd,
                          unsigned int to_submit,
                          unsigned int min_complete,
                          unsigned int flags,
                          sigset_t *sig, size_t sz);
int evio_test_uring_register(unsigned int fd, unsigned int opcode,
                             const void *arg, unsigned int nr_args);
void *evio_test_uring_mmap(void *addr, size_t length, int prot,
                           int flags, int fd, off_t offset);
int evio_test_uring_epoll_create1(int flags);
int evio_test_uring_eventfd(unsigned int initval, int flags);

void evio_test_uring_probe_tweak_params(struct io_uring_params *params);
void evio_test_uring_probe_tweak_cq_tail(uint32_t *tail, uint32_t head);
void evio_test_uring_probe_tweak_cqe_res(int *res);
bool evio_test_uring_cqe_res_override(int fd, int op, int *res);
bool evio_test_uring_probe_disable_register_probe(void);

#define EVIO_URING_SETUP(entries, params) \
    evio_test_uring_setup((entries), (params))
#define EVIO_URING_ENTER(fd, to_submit, min_complete, flags, sig, sz) \
    evio_test_uring_enter((fd), (to_submit), (min_complete), (flags), (sig), (sz))
#define EVIO_URING_REGISTER(fd, opcode, arg, nr_args) \
    evio_test_uring_register((fd), (opcode), (arg), (nr_args))
#define EVIO_URING_MMAP(addr, length, prot, flags, fd, offset) \
    evio_test_uring_mmap((addr), (length), (prot), (flags), (fd), (offset))
#define EVIO_URING_EPOLL_CREATE1(flags) \
    evio_test_uring_epoll_create1((flags))
#define EVIO_URING_EVENTFD(initval, flags) \
    evio_test_uring_eventfd((initval), (flags))

#define EVIO_URING_PROBE_TWEAK_PARAMS(params) \
    evio_test_uring_probe_tweak_params((params))
#define EVIO_URING_PROBE_TWEAK_CQ_TAIL(tail, head) \
    evio_test_uring_probe_tweak_cq_tail((tail), (head))
#define EVIO_URING_PROBE_TWEAK_CQE_RES(res) \
    evio_test_uring_probe_tweak_cqe_res((res))
#define EVIO_URING_CQE_OVERRIDE(fd, op, res) \
    ((void)evio_test_uring_cqe_res_override((fd), (op), (res)))
#define EVIO_URING_PROBE_DISABLE_REGISTER_PROBE() \
    evio_test_uring_probe_disable_register_probe()

#else // EVIO_TESTING

#define EVIO_URING_SETUP(entries, params) \
    (int)syscall(SYS_io_uring_setup, (entries), (params))
#define EVIO_URING_ENTER(fd, to_submit, min_complete, flags, sig, sz) \
    (int)syscall(SYS_io_uring_enter, (fd), (to_submit), (min_complete), (flags), (sig), (sz))
#define EVIO_URING_REGISTER(fd, opcode, arg, nr_args) \
    (int)syscall(SYS_io_uring_register, (fd), (opcode), (arg), (nr_args))
#define EVIO_URING_MMAP(addr, length, prot, flags, fd, offset) \
    mmap((addr), (length), (prot), (flags), (fd), (offset))
#define EVIO_URING_EPOLL_CREATE1(flags) \
    epoll_create1((flags))
#define EVIO_URING_EVENTFD(initval, flags) \
    eventfd((initval), (flags))

#define EVIO_URING_PROBE_TWEAK_PARAMS(params) ((void)0)
#define EVIO_URING_PROBE_TWEAK_CQ_TAIL(tail, head) ((void)0)
#define EVIO_URING_PROBE_TWEAK_CQE_RES(res) ((void)0)
#define EVIO_URING_CQE_OVERRIDE(fd, op, res) ((void)0)
#define EVIO_URING_PROBE_DISABLE_REGISTER_PROBE() (false)

#endif // EVIO_TESTING
