#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "evio_core.h"
#include "evio_uring.h"
#include "evio_uring_sys.h"

struct evio_uring {
    struct epoll_event events[EVIO_URING_EVENTS]; /**< Local cache for epoll_event structs. */
    uint32_t *sqhead;       /**< Pointer to the submission queue head. */
    uint32_t *cqhead;       /**< Pointer to the completion queue head. */
    uint32_t *sqtail;       /**< Pointer to the submission queue tail. */
    uint32_t *cqtail;       /**< Pointer to the completion queue tail. */
    uint32_t sqmask;        /**< Mask for the submission queue. */
    uint32_t cqmask;        /**< Mask for the completion queue. */
    void *ptr;              /**< Pointer to the main mmap'd ring buffer. */
    struct io_uring_cqe *cqe; /**< Pointer to the start of the CQE array. */
    struct io_uring_sqe *sqe; /**< Pointer to the start of the SQE array. */
    size_t maxlen;          /**< Length of the main mmap'd region. */
    size_t sqelen;          /**< Length of the SQE mmap'd region. */
    int fd;                 /**< The io_uring file descriptor. */
};

_Static_assert(EVIO_URING_EVENTS <= 256,
               "EVIO_URING_EVENTS must fit in user_data slot encoding");

/** @brief Atomic load with acquire memory ordering. */
#define evio_uring_load(ptr)      __atomic_load_n((ptr), __ATOMIC_ACQUIRE)

/** @brief Atomic store with release memory ordering. */
#define evio_uring_store(ptr, v)  __atomic_store_n((ptr), (v), __ATOMIC_RELEASE)

/**
 * @brief A thin wrapper around the `io_uring_setup` syscall.
 * @param entries The number of entries for the ring.
 * @param params The setup parameters.
 * @return The io_uring file descriptor on success, or a negative error code.
 */
static inline __evio_nodiscard
int evio_uring_setup(unsigned int entries,
                     struct io_uring_params *params)
{
    return EVIO_URING_SETUP(entries, params);
}

/**
 * @brief A thin wrapper around the `io_uring_enter` syscall.
 * @param fd The io_uring file descriptor.
 * @param to_submit Number of SQEs to submit.
 * @param min_complete Number of CQEs to wait for.
 * @param flags Flags for the enter operation.
 * @param sig Signal mask.
 * @param sz Size of the signal mask.
 * @return 0 on success, or a negative error code.
 */
static inline __evio_nodiscard
int evio_uring_enter(unsigned int fd,
                     unsigned int to_submit,
                     unsigned int min_complete,
                     unsigned int flags,
                     sigset_t *sig, size_t sz)
{
    return EVIO_URING_ENTER(fd, to_submit, min_complete, flags, sig, sz);
}

/**
 * @brief Submits pending `io_uring` operations and waits for their completion.
 * @param loop The event loop.
 */
static void evio_uring_submit_and_wait(evio_loop *loop)
{
    evio_uring *iou = loop->iou;

    // GCOVR_EXCL_START
    EVIO_ASSERT(iou && iou->fd >= 0);
    // GCOVR_EXCL_STOP

    unsigned int n = loop->iou_count;
    // GCOVR_EXCL_START
    EVIO_ASSERT(n);
    // GCOVR_EXCL_STOP

    for (;;) { // GCOVR_EXCL_LINE
        int ret = evio_uring_enter(iou->fd, n, n, IORING_ENTER_GETEVENTS,
                                   &loop->sigmask, sizeof(loop->sigmask));
        // GCOVR_EXCL_START
        if (__evio_unlikely(ret < 0)) {
            int err = ret == -1 ? errno : -ret;
            if (err == EINTR || err == EAGAIN) {
                continue;
            }
            EVIO_ABORT("io_uring_enter() failed, error %d: %s\n", err, EVIO_STRERROR(err));
        }
        if (__evio_unlikely((uint32_t)ret != n)) {
            EVIO_ABORT("io_uring_enter() failed, result %d/%u\n", ret, n);
        }
        // GCOVR_EXCL_STOP
        break;
    }

    loop->iou_count = 0;
}

void evio_uring_ctl(evio_loop *loop, int op, int fd, const struct epoll_event *ev)
{
    evio_uring *iou = loop->iou;

    // GCOVR_EXCL_START
    EVIO_ASSERT(iou && iou->fd >= 0);

    EVIO_ASSERT(op == EPOLL_CTL_ADD ||
                op == EPOLL_CTL_MOD);
    // GCOVR_EXCL_STOP

    uint32_t mask = iou->sqmask;
    uint32_t tail = *iou->sqtail;
    uint32_t head = evio_uring_load(iou->sqhead);

    if (__evio_unlikely(((tail + 1) & mask) == (head & mask))) {
        evio_uring_flush(loop);
        head = evio_uring_load(iou->sqhead);
    }

    uint32_t slot = tail & mask;

    struct epoll_event *event = &iou->events[slot];
    *event = *ev;

    struct io_uring_sqe *sqe = &iou->sqe[slot];
    *sqe = (struct io_uring_sqe) {
        .opcode     = IORING_OP_EPOLL_CTL,
        .fd         = loop->fd,
        .off        = fd,
        .addr       = (uintptr_t)event,
        .len        = op,
        .user_data  = ((uint64_t)fd) |
                      ((uint64_t)op << 32) |
                      ((uint64_t)slot << 34),
    };

    evio_uring_store(iou->sqtail, ++tail);
    ++loop->iou_count;
}

void evio_uring_flush(evio_loop *loop)
{
    evio_uring *iou = loop->iou;

    // GCOVR_EXCL_START
    EVIO_ASSERT(iou && iou->fd >= 0);
    // GCOVR_EXCL_STOP

    while (loop->iou_count) {
        evio_uring_submit_and_wait(loop);

        uint32_t head = *iou->cqhead;
        uint32_t tail = evio_uring_load(iou->cqtail);

        for (; head != tail; ++head) {
            uint32_t mask = iou->cqmask;
            uint32_t slot = head & mask;

            const struct io_uring_cqe *cqe = &iou->cqe[slot];

            uint32_t fd32 = cqe->user_data & UINT32_MAX;
            // GCOVR_EXCL_START
            if (__evio_unlikely(fd32 >= loop->fds.count)) {
                EVIO_ABORT("Invalid fd %u\n", fd32);
            }
            // GCOVR_EXCL_STOP

            int fd = fd32;
            int op = (cqe->user_data >> 32) & 3;
            // GCOVR_EXCL_START
            if (__evio_unlikely(op != EPOLL_CTL_ADD &&
                                op != EPOLL_CTL_MOD)) {
                EVIO_ABORT("Invalid fd %d op %d\n", fd, op);
            }
            // GCOVR_EXCL_STOP

            slot = (cqe->user_data >> 34) & 0xFF;
            // GCOVR_EXCL_START
            if (__evio_unlikely(slot >= EVIO_URING_EVENTS)) {
                EVIO_ABORT("Invalid fd %d slot %u\n", fd, slot);
            }
            // GCOVR_EXCL_STOP

            const struct epoll_event *ev = &iou->events[slot];

            int res = cqe->res;
            EVIO_URING_CQE_OVERRIDE(fd, op, &res);

            if (__evio_likely(res == 0)) {
                continue;
            }

            switch (res) {
                case -EEXIST:
                    if (op == EPOLL_CTL_ADD) {
                        evio_uring_ctl(loop, EPOLL_CTL_MOD, fd, ev);
                        break;
                    }
                    __evio_fallthrough;

                case -ENOENT:
                    if (op == EPOLL_CTL_MOD && res == -ENOENT) {
                        evio_uring_ctl(loop, EPOLL_CTL_ADD, fd, ev);
                        break;
                    }
                    __evio_fallthrough;

                case -EPERM:
                    if (res == -EPERM) {
                        evio_queue_fd_error(loop, fd);
                        break;
                    }
                    __evio_fallthrough;

                default:
                    loop->fds.ptr[fd].gen--;
                    evio_queue_fd_errors(loop, fd);
                    break;
            }
        }

        if (*iou->cqhead != head) { // GCOVR_EXCL_LINE
            evio_uring_store(iou->cqhead, head);
        }
    }
}

/**
 * @brief Runtime probe to check if IORING_OP_EPOLL_CTL is supported.
 * @return 1 if supported, 0 if unsupported, -1 if indeterminate.
 */
static int evio_uring_probe_epoll_ctl(void)
{
    struct io_uring_params params = { 0 };
    params.flags = IORING_SETUP_CLAMP;

    EVIO_URING_PROBE_TWEAK_PARAMS(&params);

    int fd = evio_uring_setup(2, &params);
    if (__evio_unlikely(fd < 0)) {
        int err = fd == -1 ? errno : -fd;
        if (err == ENOSYS) {
            return 0;
        }
        return -1;
    }

    EVIO_URING_PROBE_TWEAK_PARAMS(&params);

    /*
     * Prefer the official PROBE API when available (no mmap assumptions).
     * If it's not supported by the running kernel, fall back to a minimal
     * submit-and-check sequence.
     */
    bool supported = false;
    int result = -1;

#ifdef IORING_REGISTER_PROBE
    if (__evio_likely(!EVIO_URING_PROBE_DISABLE_REGISTER_PROBE())) {
        unsigned int ops_len = 256;
#ifdef IORING_OP_LAST
        ops_len = (unsigned int)IORING_OP_LAST;
#endif
        size_t bytes = sizeof(struct io_uring_probe) +
                       ops_len * sizeof(struct io_uring_probe_op);
        struct io_uring_probe *probe = __builtin_alloca(bytes);
        memset(probe, 0, bytes);
        probe->ops_len = ops_len;

        int ret = EVIO_URING_REGISTER(fd, IORING_REGISTER_PROBE, probe, ops_len);
        if (ret == 0) {
            for (unsigned int i = 0; i < probe->ops_len; ++i) {
                const struct io_uring_probe_op *op = &probe->ops[i];
                if (op->op == IORING_OP_EPOLL_CTL) {
                    supported = (op->flags & 1u) != 0;
                    break;
                }
            }

            close(fd);
            return supported ? 1 : 0;
        }
    }
#endif

    size_t sqlen = params.sq_off.array + (params.sq_entries * sizeof(uint32_t));
    size_t cqlen = params.cq_off.cqes + (params.cq_entries * sizeof(struct io_uring_cqe));
    size_t sqelen = params.sq_entries * sizeof(struct io_uring_sqe);

    uint8_t *sqptr = NULL;
    uint8_t *cqptr = NULL;
    size_t sqmap_len = 0;
    size_t cqmap_len = 0;

#ifdef IORING_FEAT_SINGLE_MMAP
    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        size_t maxlen = sqlen > cqlen ? sqlen : cqlen;
        sqptr = EVIO_URING_MMAP(NULL, maxlen, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_POPULATE,
                                fd, IORING_OFF_SQ_RING);
        if (__evio_unlikely(sqptr == MAP_FAILED)) {
            close(fd);
            return false;
        }
        cqptr = sqptr;
        sqmap_len = maxlen;
    } else
#endif
    {
        sqptr = EVIO_URING_MMAP(NULL, sqlen, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_POPULATE,
                                fd, IORING_OFF_SQ_RING);
        if (__evio_unlikely(sqptr == MAP_FAILED)) {
            close(fd);
            return false;
        }
        sqmap_len = sqlen;

        cqptr = EVIO_URING_MMAP(NULL, cqlen, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_POPULATE,
                                fd, IORING_OFF_CQ_RING);
        if (__evio_unlikely(cqptr == MAP_FAILED)) {
            munmap(sqptr, sqmap_len);
            close(fd);
            return false;
        }
        cqmap_len = cqlen;
    }

    struct io_uring_sqe *sqe = EVIO_URING_MMAP(NULL, sqelen, PROT_READ | PROT_WRITE,
                                               MAP_SHARED | MAP_POPULATE,
                                               fd, IORING_OFF_SQES);
    if (__evio_unlikely(sqe == MAP_FAILED)) {
        if (cqptr != sqptr) {
            munmap(cqptr, cqmap_len);
        }
        munmap(sqptr, sqmap_len);
        close(fd);
        return false;
    }

    uint32_t *sqtail = (uint32_t *)(sqptr + params.sq_off.tail);
    uint32_t *cqhead = (uint32_t *)(cqptr + params.cq_off.head);
    uint32_t *cqtail = (uint32_t *)(cqptr + params.cq_off.tail);
    uint32_t cqmask = *(uint32_t *)(cqptr + params.cq_off.ring_mask);
    struct io_uring_cqe *cqe = (struct io_uring_cqe *)(cqptr + params.cq_off.cqes);

    if (params.sq_off.array) {
        uint32_t *sqarray = (uint32_t *)(sqptr + params.sq_off.array);
        sqarray[0] = 0;
    }

    int probe_epfd = EVIO_URING_EPOLL_CREATE1(EPOLL_CLOEXEC);
    if (__evio_unlikely(probe_epfd < 0)) {
        munmap(sqe, sqelen);
        if (cqptr != sqptr) {
            munmap(cqptr, cqmap_len);
        }
        munmap(sqptr, sqmap_len);
        close(fd);
        return false;
    }

    struct epoll_event probe_ev = { .events = EPOLLIN, .data.u64 = 0 };
    int probe_fd = EVIO_URING_EVENTFD(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (__evio_unlikely(probe_fd < 0)) {
        close(probe_epfd);
        munmap(sqe, sqelen);
        if (cqptr != sqptr) {
            munmap(cqptr, cqmap_len);
        }
        munmap(sqptr, sqmap_len);
        close(fd);
        return false;
    }

    sqe[0] = (struct io_uring_sqe) {
        .opcode     = IORING_OP_EPOLL_CTL,
        .fd         = probe_epfd,
        .off        = probe_fd,
        .addr       = (uintptr_t)&probe_ev,
        .len        = EPOLL_CTL_ADD,
        .user_data  = 0,
    };

    evio_uring_store(sqtail, 1);

    int ret = evio_uring_enter(fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    if (__evio_likely(ret == 1)) {
        uint32_t head = evio_uring_load(cqhead);
        uint32_t tail = evio_uring_load(cqtail);
        EVIO_URING_PROBE_TWEAK_CQ_TAIL(&tail, head);
        if (head != tail) {
            int res = cqe[head & cqmask].res;
            EVIO_URING_PROBE_TWEAK_CQE_RES(&res);
            if (res == 0) {
                supported = true;
                result = 1;
            } else if (res == -EINVAL) {
                supported = false;
                result = 0;
            }
        }
    }

    close(probe_fd);
    close(probe_epfd);
    munmap(sqe, sqelen);
    if (cqptr != sqptr) {
        munmap(cqptr, cqmap_len);
    }
    munmap(sqptr, sqmap_len);
    close(fd);
    return result;
}

#ifndef EVIO_TESTING

// GCOVR_EXCL_START
static bool evio_uring_probe_epoll_ctl_cached(void)
{
    static _Atomic int cached = -1;

    int v = atomic_load_explicit(&cached, memory_order_acquire);
    if (__evio_likely(v >= 0)) {
        return v != 0;
    }

    int probe = evio_uring_probe_epoll_ctl();
    if (probe >= 0) {
        atomic_store_explicit(&cached, probe ? 1 : 0, memory_order_release);
        return probe != 0;
    }

    return false;
}
// GCOVR_EXCL_STOP

#endif

evio_uring *evio_uring_new(void)
{
#ifdef EVIO_TESTING
    if (__evio_unlikely(evio_uring_probe_epoll_ctl() != 1)) {
        return NULL;
    }
#else // GCOVR_EXCL_START
    if (__evio_unlikely(!evio_uring_probe_epoll_ctl_cached())) {
        return NULL;
    }
#endif // GCOVR_EXCL_STOP

    struct io_uring_params params = { 0 };

    params.flags |= IORING_SETUP_CLAMP;

#ifdef IORING_SETUP_SUBMIT_ALL
    params.flags |= IORING_SETUP_SUBMIT_ALL;
#endif

#ifdef IORING_SETUP_COOP_TASKRUN
    params.flags |= IORING_SETUP_COOP_TASKRUN;
#endif

#ifdef IORING_SETUP_NO_SQARRAY
    params.flags |= IORING_SETUP_NO_SQARRAY;
#endif

    int fd = evio_uring_setup(EVIO_URING_EVENTS, &params);
    // GCOVR_EXCL_START
    if (__evio_unlikely(fd < 0)) {
        int err = fd == -1 ? errno : -fd;
        if (err != EINVAL) {
            return NULL;
        }

        memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_CLAMP;

        fd = evio_uring_setup(EVIO_URING_EVENTS, &params);
        if (__evio_unlikely(fd < 0)) {
            return NULL;
        }
    }
    // GCOVR_EXCL_STOP

    const uint32_t features = IORING_FEAT_SINGLE_MMAP |
                              IORING_FEAT_NODROP |
                              IORING_FEAT_SUBMIT_STABLE |
                              IORING_FEAT_RSRC_TAGS;
    // GCOVR_EXCL_START
    if (__evio_unlikely((~params.features) & features)) {
        close(fd);
        return NULL;
    }
    // GCOVR_EXCL_STOP

    size_t sqlen = params.sq_off.array + (params.sq_entries * sizeof(uint32_t));
    size_t cqlen = params.cq_off.cqes + (params.cq_entries * sizeof(struct io_uring_cqe));
    size_t maxlen = sqlen > cqlen ? sqlen : cqlen;
    size_t sqelen = params.sq_entries * sizeof(struct io_uring_sqe);

    uint8_t *ptr = EVIO_URING_MMAP(NULL, maxlen,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_POPULATE,
                                   fd, IORING_OFF_SQ_RING);
    // GCOVR_EXCL_START
    if (__evio_unlikely(ptr == MAP_FAILED)) {
        close(fd);
        return NULL;
    }
    // GCOVR_EXCL_STOP

    void *sqe = EVIO_URING_MMAP(NULL, sqelen,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_POPULATE,
                                fd, IORING_OFF_SQES);
    // GCOVR_EXCL_START
    if (__evio_unlikely(sqe == MAP_FAILED)) {
        munmap(ptr, maxlen);
        close(fd);
        return NULL;
    }
    // GCOVR_EXCL_STOP

    evio_uring *iou = evio_malloc(sizeof(*iou));
    *iou = (evio_uring) {
        .sqhead     = (uint32_t *)(ptr + params.sq_off.head),
        .cqhead     = (uint32_t *)(ptr + params.cq_off.head),
        .sqtail     = (uint32_t *)(ptr + params.sq_off.tail),
        .cqtail     = (uint32_t *)(ptr + params.cq_off.tail),
        .sqmask     = *(uint32_t *)(ptr + params.sq_off.ring_mask),
        .cqmask     = *(uint32_t *)(ptr + params.cq_off.ring_mask),
        .ptr        = ptr,
        .cqe        = (struct io_uring_cqe *)(ptr + params.cq_off.cqes),
        .sqe        = (struct io_uring_sqe *)(sqe),
        .maxlen     = maxlen,
        .sqelen     = sqelen,
        .fd         = fd,
    };

    // GCOVR_EXCL_START
    if (params.sq_off.array) {
        uint32_t *sqarray = (uint32_t *)(ptr + params.sq_off.array);
        for (uint32_t i = 0; i <= iou->sqmask; ++i) {
            sqarray[i] = i;
        }
    }
    // GCOVR_EXCL_STOP

    return iou;
}

void evio_uring_free(evio_uring *iou)
{
    munmap(iou->ptr, iou->maxlen);
    munmap(iou->sqe, iou->sqelen);

    close(iou->fd);
    evio_free(iou);
}
