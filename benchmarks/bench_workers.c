#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/eventfd.h>

#include <ev.h>
enum {
    LIBEV_READ  = EV_READ,
    LIBEV_WRITE = EV_WRITE,
};
#undef EV_READ
#undef EV_WRITE

#include <event2/event.h>
enum {
    LIBEVENT_READ  = EV_READ,
    LIBEVENT_WRITE = EV_WRITE,
};
#include <uv.h>

#include "evio.h"
#include "bench.h"

#define DEF_WORKERS     4u
#define DEF_CONNS       1024u
#define DEF_K           8u

#define MAX_WORKERS     16u
#define MAX_CONNS       65536u
#define MAX_K           1024u

#define MSG_SIZE        64u
#define MSGS_PER_CONN   1024u
#define IO_BATCH        64u

static unsigned int env_u32(const char *name, unsigned int def, unsigned int max)
{
    const char *s = getenv(name);
    if (!s || !*s) {
        return def;
    }

    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end) {
        return def;
    }

    if (!v) {
        return def;
    }
    if (v > max) {
        return max;
    }
    return (unsigned int)v;
}

static int set_nonblock_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return -1;
    }

    return 0;
}

static int socketpair_nb(int fds[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return -1;
    }
    if (set_nonblock_cloexec(fds[0]) != 0 || set_nonblock_cloexec(fds[1]) != 0) {
        int err = errno;
        close(fds[0]);
        close(fds[1]);
        errno = err;
        return -1;
    }
    return 0;
}

static unsigned int cap_conns_by_rlimit(unsigned int workers, unsigned int conns)
{
    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) != 0) {
        return conns;
    }
    if (lim.rlim_cur == RLIM_INFINITY) {
        return conns;
    }

    unsigned long long cur = lim.rlim_cur;
    unsigned long long margin = 256;
    unsigned long long denom = (unsigned long long)workers * 2ull;
    if (!denom) {
        denom = 1;
    }

    unsigned long long maxc = (cur > margin) ? ((cur - margin) / denom) : 1;
    if (!maxc) {
        maxc = 1;
    }
    if ((unsigned long long)conns > maxc) {
        return (unsigned int)maxc;
    }
    return conns;
}

// --- evio ---
typedef struct evio_worker evio_worker;

typedef struct {
    evio_poll io;
    evio_worker *w;
    int srv_fd;
    int cli_fd;
    size_t out_pending;
    size_t cli_send_accum;
    size_t cli_recv_accum;
    bool want_write;
} evio_conn;

struct evio_worker {
    pthread_t thr;
    pthread_barrier_t *ready;
    pthread_barrier_t *start;
    pthread_barrier_t *finish;
    evio_worker *all;
    unsigned int workers;
    unsigned int idx;
    unsigned int conns;
    unsigned int k;
    bool use_uring;

    evio_loop *loop;
    evio_async async;
    _Atomic bool alive;
    _Atomic unsigned int churn_reqs;
    unsigned int churn_pending;
    unsigned int churn_seq;

    evio_conn *conn;
    uint64_t msgs_target;
    uint64_t sent_msgs;
    uint64_t done_msgs;
};

static void evio_conn_update_events(evio_loop *loop, evio_conn *c)
{
    evio_mask want = EVIO_READ | (c->out_pending ? EVIO_WRITE : 0);
    if (!c->want_write && (want & EVIO_WRITE)) {
        c->want_write = true;
        evio_poll_change(loop, &c->io, c->srv_fd, want);
        return;
    }
    if (c->want_write && !(want & EVIO_WRITE)) {
        c->want_write = false;
        evio_poll_change(loop, &c->io, c->srv_fd, want);
        return;
    }
}

static void evio_conn_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_conn *c = base->data;
    char buf[4096];

    if (emask & EVIO_READ) {
        for (;;) {
            ssize_t n = read(c->srv_fd, buf, sizeof(buf));
            if (n > 0) {
                c->out_pending += (size_t)n;
                continue;
            }
            if (n < 0 && (errno == EINTR)) {
                continue;
            }
            break;
        }
    }

    if (emask & EVIO_WRITE) {
        static const char out[4096] = { 0 };
        while (c->out_pending) {
            size_t todo = c->out_pending;
            if (todo > sizeof(out)) {
                todo = sizeof(out);
            }

            ssize_t n = write(c->srv_fd, out, todo);
            if (n > 0) {
                c->out_pending -= (size_t)n;
                continue;
            }
            if (n < 0 && (errno == EINTR)) {
                continue;
            }
            break;
        }
    }

    evio_conn_update_events(loop, c);
}

static void evio_conn_reopen(evio_loop *loop, evio_conn *c)
{
    int fds[2];
    if (socketpair_nb(fds) != 0) {
        int err = errno;
        EVIO_ABORT("socketpair() failed, error %d: %s\n", err, EVIO_STRERROR(err));
    }

    int old_srv = c->srv_fd;
    int old_cli = c->cli_fd;

    c->srv_fd = fds[0];
    c->cli_fd = fds[1];
    c->out_pending = 0;
    c->cli_send_accum = 0;
    c->cli_recv_accum = 0;
    c->want_write = false;

    evio_poll_change(loop, &c->io, c->srv_fd, EVIO_READ);

    close(old_srv);
    close(old_cli);
}

static void evio_async_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    (void)emask;

    evio_worker *w = base->data;
    w->churn_pending += atomic_exchange_explicit(&w->churn_reqs, 0, memory_order_acq_rel);
}

static void *evio_workers_thread(void *ptr)
{
    evio_worker *w = ptr;
    w->loop = evio_loop_new(w->use_uring ? EVIO_FLAG_URING : EVIO_FLAG_NONE);
    if (!w->loop) {
        abort();
    }

    w->conn = calloc(w->conns, sizeof(*w->conn));
    if (!w->conn) {
        abort();
    }

    for (unsigned int i = 0; i < w->conns; ++i) {
        int fds[2];
        if (socketpair_nb(fds) != 0) {
            abort();
        }

        evio_conn *c = &w->conn[i];
        c->w = w;
        c->srv_fd = fds[0];
        c->cli_fd = fds[1];
        c->out_pending = 0;
        c->cli_send_accum = 0;
        c->cli_recv_accum = 0;
        c->want_write = false;

        evio_poll_init(&c->io, evio_conn_cb, c->srv_fd, EVIO_READ);
        c->io.data = c;
        evio_poll_start(w->loop, &c->io);
    }

    evio_async_init(&w->async, evio_async_cb);
    w->async.data = w;
    atomic_init(&w->churn_reqs, 0);
    w->churn_pending = 0;
    w->churn_seq = 0;
    evio_async_start(w->loop, &w->async);
    atomic_store_explicit(&w->alive, true, memory_order_release);

    pthread_barrier_wait(w->ready);
    pthread_barrier_wait(w->start);

    uint64_t async_every = (uint64_t)w->k * (uint64_t)w->conns;
    uint64_t next_async = async_every ? async_every : UINT64_MAX;

    char in[MSG_SIZE];
    memset(in, 'x', sizeof(in));

    unsigned int seq = 0;
    while (w->done_msgs < w->msgs_target) {
        unsigned int idxs[IO_BATCH];
        unsigned int nidx = 0;

        for (; nidx < IO_BATCH && w->sent_msgs < w->msgs_target; ++nidx) {
            unsigned int idx = seq++ % w->conns;
            idxs[nidx] = idx;

            evio_conn *c = &w->conn[idx];
            ssize_t n = write(c->cli_fd, in, sizeof(in));
            if (n > 0) {
                c->cli_send_accum += (size_t)n;
                while (c->cli_send_accum >= MSG_SIZE && w->sent_msgs < w->msgs_target) {
                    c->cli_send_accum -= MSG_SIZE;
                    ++w->sent_msgs;
                }
            }
        }

        for (int i = 0; i < 3; ++i) {
            evio_run(w->loop, EVIO_RUN_NOWAIT);
        }

        for (unsigned int i = 0; i < nidx; ++i) {
            unsigned int idx = idxs[i];
            evio_conn *c = &w->conn[idx];
            for (;;) {
                char out[4096];
                ssize_t n = read(c->cli_fd, out, sizeof(out));
                if (n > 0) {
                    c->cli_recv_accum += (size_t)n;
                    while (c->cli_recv_accum >= MSG_SIZE && w->done_msgs < w->msgs_target) {
                        c->cli_recv_accum -= MSG_SIZE;
                        ++w->done_msgs;
                    }
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
        }

        while (w->churn_pending && w->sent_msgs == w->done_msgs) {
            unsigned int idx = w->churn_seq++ % w->conns;
            evio_conn_reopen(w->loop, &w->conn[idx]);
            --w->churn_pending;
        }

        if (w->done_msgs >= next_async && w->workers > 1) {
            unsigned int dst = (w->idx + 1) % w->workers;
            evio_worker *t = &w->all[dst];
            if (atomic_load_explicit(&t->alive, memory_order_acquire)) {
                atomic_fetch_add_explicit(&t->churn_reqs, 1, memory_order_release);
                evio_async_send(t->loop, &t->async);
            }
            next_async += async_every;
        }
    }

    atomic_store_explicit(&w->alive, false, memory_order_release);
    pthread_barrier_wait(w->finish);

    evio_async_stop(w->loop, &w->async);

    for (unsigned int i = 0; i < w->conns; ++i) {
        evio_conn *c = &w->conn[i];
        evio_poll_stop(w->loop, &c->io);
        close(c->srv_fd);
        close(c->cli_fd);
    }
    free(w->conn);
    evio_loop_free(w->loop);
    return NULL;
}

static void bench_evio_workers(unsigned int workers, unsigned int conns, unsigned int k, bool use_uring)
{
    pthread_barrier_t ready;
    pthread_barrier_t start;
    pthread_barrier_t finish;
    if (pthread_barrier_init(&ready, NULL, workers + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&start, NULL, workers + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&finish, NULL, workers) != 0) {
        abort();
    }

    evio_worker *w = calloc(workers, sizeof(*w));
    if (!w) {
        abort();
    }

    for (unsigned int i = 0; i < workers; ++i) {
        w[i].ready = &ready;
        w[i].start = &start;
        w[i].finish = &finish;
        w[i].all = w;
        w[i].workers = workers;
        w[i].idx = i;
        w[i].conns = conns;
        w[i].k = k;
        w[i].use_uring = use_uring;
        w[i].msgs_target = (uint64_t)conns * MSGS_PER_CONN;
        if (pthread_create(&w[i].thr, NULL, evio_workers_thread, &w[i]) != 0) {
            abort();
        }
    }

    pthread_barrier_wait(&ready);

    uint64_t start_ns = get_time_ns();
    pthread_barrier_wait(&start);

    for (unsigned int i = 0; i < workers; ++i) {
        pthread_join(w[i].thr, NULL);
    }
    uint64_t end_ns = get_time_ns();

    pthread_barrier_destroy(&ready);
    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&finish);

    uint64_t ops = (uint64_t)workers * (uint64_t)conns * MSGS_PER_CONN;
    print_benchmark("workers", use_uring ? "evio-uring" : "evio", end_ns - start_ns, ops);
    free(w);
}

// --- libev ---
typedef struct libev_worker libev_worker;

typedef struct {
    ev_io io;
    libev_worker *w;
    int srv_fd;
    int cli_fd;
    size_t out_pending;
    size_t cli_send_accum;
    size_t cli_recv_accum;
    bool want_write;
} libev_conn;

struct libev_worker {
    pthread_t thr;
    pthread_barrier_t *ready;
    pthread_barrier_t *start;
    pthread_barrier_t *finish;
    libev_worker *all;
    unsigned int workers;
    unsigned int idx;
    unsigned int conns;
    unsigned int k;

    struct ev_loop *loop;
    ev_async async;
    _Atomic bool alive;
    _Atomic unsigned int churn_reqs;
    unsigned int churn_pending;
    unsigned int churn_seq;

    libev_conn *conn;
    uint64_t msgs_target;
    uint64_t sent_msgs;
    uint64_t done_msgs;
};

static void libev_conn_update_events(struct ev_loop *loop, libev_conn *c)
{
    int want = LIBEV_READ | (c->out_pending ? LIBEV_WRITE : 0);
    if (!c->want_write && (want & LIBEV_WRITE)) {
        c->want_write = true;
        ev_io_stop(loop, &c->io);
        ev_io_set(&c->io, c->srv_fd, want);
        ev_io_start(loop, &c->io);
        return;
    }
    if (c->want_write && !(want & LIBEV_WRITE)) {
        c->want_write = false;
        ev_io_stop(loop, &c->io);
        ev_io_set(&c->io, c->srv_fd, want);
        ev_io_start(loop, &c->io);
        return;
    }
}

static void libev_conn_cb(struct ev_loop *loop, ev_io *io, int revents)
{
    libev_conn *c = io->data;
    char buf[4096];

    if (revents & LIBEV_READ) {
        for (;;) {
            ssize_t n = read(c->srv_fd, buf, sizeof(buf));
            if (n > 0) {
                c->out_pending += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    if (revents & LIBEV_WRITE) {
        static const char out[4096] = { 0 };
        while (c->out_pending) {
            size_t todo = c->out_pending;
            if (todo > sizeof(out)) {
                todo = sizeof(out);
            }

            ssize_t n = write(c->srv_fd, out, todo);
            if (n > 0) {
                c->out_pending -= (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    libev_conn_update_events(loop, c);
}

static void libev_conn_reopen(struct ev_loop *loop, libev_conn *c)
{
    int fds[2];
    if (socketpair_nb(fds) != 0) {
        int err = errno;
        fprintf(stderr, "socketpair() failed, error %d: %s\n", err, strerror(err));
        abort();
    }

    ev_io_stop(loop, &c->io);
    close(c->srv_fd);
    close(c->cli_fd);

    c->srv_fd = fds[0];
    c->cli_fd = fds[1];
    c->out_pending = 0;
    c->cli_send_accum = 0;
    c->cli_recv_accum = 0;
    c->want_write = false;

    ev_io_set(&c->io, c->srv_fd, LIBEV_READ);
    ev_io_start(loop, &c->io);
}

static void libev_async_cb(struct ev_loop *loop, ev_async *a, int revents)
{
    (void)revents;

    libev_worker *w = a->data;
    w->churn_pending += atomic_exchange_explicit(&w->churn_reqs, 0, memory_order_acq_rel);
}

static void *libev_workers_thread(void *ptr)
{
    libev_worker *w = ptr;
    w->loop = ev_loop_new(0);
    if (!w->loop) {
        abort();
    }

    w->conn = calloc(w->conns, sizeof(*w->conn));
    if (!w->conn) {
        abort();
    }

    for (unsigned int i = 0; i < w->conns; ++i) {
        int fds[2];
        if (socketpair_nb(fds) != 0) {
            abort();
        }

        libev_conn *c = &w->conn[i];
        c->w = w;
        c->srv_fd = fds[0];
        c->cli_fd = fds[1];
        c->out_pending = 0;
        c->cli_send_accum = 0;
        c->cli_recv_accum = 0;
        c->want_write = false;

        ev_io_init(&c->io, libev_conn_cb, c->srv_fd, LIBEV_READ);
        c->io.data = c;
        ev_io_start(w->loop, &c->io);
    }

    atomic_init(&w->churn_reqs, 0);
    w->churn_pending = 0;
    w->churn_seq = 0;
    ev_async_init(&w->async, libev_async_cb);
    w->async.data = w;
    ev_async_start(w->loop, &w->async);
    atomic_store_explicit(&w->alive, true, memory_order_release);

    pthread_barrier_wait(w->ready);
    pthread_barrier_wait(w->start);

    uint64_t async_every = (uint64_t)w->k * (uint64_t)w->conns;
    uint64_t next_async = async_every ? async_every : UINT64_MAX;

    char in[MSG_SIZE];
    memset(in, 'x', sizeof(in));

    unsigned int seq = 0;
    while (w->done_msgs < w->msgs_target) {
        unsigned int idxs[IO_BATCH];
        unsigned int nidx = 0;

        for (; nidx < IO_BATCH && w->sent_msgs < w->msgs_target; ++nidx) {
            unsigned int idx = seq++ % w->conns;
            idxs[nidx] = idx;

            libev_conn *c = &w->conn[idx];
            ssize_t n = write(c->cli_fd, in, sizeof(in));
            if (n > 0) {
                c->cli_send_accum += (size_t)n;
                while (c->cli_send_accum >= MSG_SIZE && w->sent_msgs < w->msgs_target) {
                    c->cli_send_accum -= MSG_SIZE;
                    ++w->sent_msgs;
                }
            }
        }

        for (int i = 0; i < 3; ++i) {
            ev_run(w->loop, EVRUN_NOWAIT);
        }

        for (unsigned int i = 0; i < nidx; ++i) {
            unsigned int idx = idxs[i];
            libev_conn *c = &w->conn[idx];
            for (;;) {
                char out[4096];
                ssize_t n = read(c->cli_fd, out, sizeof(out));
                if (n > 0) {
                    c->cli_recv_accum += (size_t)n;
                    while (c->cli_recv_accum >= MSG_SIZE && w->done_msgs < w->msgs_target) {
                        c->cli_recv_accum -= MSG_SIZE;
                        ++w->done_msgs;
                    }
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
        }

        while (w->churn_pending && w->sent_msgs == w->done_msgs) {
            unsigned int idx = w->churn_seq++ % w->conns;
            libev_conn_reopen(w->loop, &w->conn[idx]);
            --w->churn_pending;
        }

        if (w->done_msgs >= next_async && w->workers > 1) {
            unsigned int dst = (w->idx + 1) % w->workers;
            libev_worker *t = &w->all[dst];
            if (atomic_load_explicit(&t->alive, memory_order_acquire)) {
                atomic_fetch_add_explicit(&t->churn_reqs, 1, memory_order_release);
                ev_async_send(t->loop, &t->async);
            }
            next_async += async_every;
        }
    }

    atomic_store_explicit(&w->alive, false, memory_order_release);
    pthread_barrier_wait(w->finish);

    ev_async_stop(w->loop, &w->async);

    for (unsigned int i = 0; i < w->conns; ++i) {
        libev_conn *c = &w->conn[i];
        ev_io_stop(w->loop, &c->io);
        close(c->srv_fd);
        close(c->cli_fd);
    }
    free(w->conn);

    ev_loop_destroy(w->loop);
    return NULL;
}

static void bench_libev_workers(unsigned int workers, unsigned int conns, unsigned int k)
{
    pthread_barrier_t ready;
    pthread_barrier_t start;
    pthread_barrier_t finish;
    if (pthread_barrier_init(&ready, NULL, workers + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&start, NULL, workers + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&finish, NULL, workers) != 0) {
        abort();
    }

    libev_worker *w = calloc(workers, sizeof(*w));
    if (!w) {
        abort();
    }

    for (unsigned int i = 0; i < workers; ++i) {
        w[i].ready = &ready;
        w[i].start = &start;
        w[i].finish = &finish;
        w[i].all = w;
        w[i].workers = workers;
        w[i].idx = i;
        w[i].conns = conns;
        w[i].k = k;
        w[i].msgs_target = (uint64_t)conns * MSGS_PER_CONN;
        if (pthread_create(&w[i].thr, NULL, libev_workers_thread, &w[i]) != 0) {
            abort();
        }
    }

    pthread_barrier_wait(&ready);

    uint64_t start_ns = get_time_ns();
    pthread_barrier_wait(&start);

    for (unsigned int i = 0; i < workers; ++i) {
        pthread_join(w[i].thr, NULL);
    }
    uint64_t end_ns = get_time_ns();

    pthread_barrier_destroy(&ready);
    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&finish);

    uint64_t ops = (uint64_t)workers * (uint64_t)conns * MSGS_PER_CONN;
    print_benchmark("workers", "libev", end_ns - start_ns, ops);
    free(w);
}

// --- libevent ---
typedef struct libevent_worker libevent_worker;

typedef struct {
    libevent_worker *w;
    struct event *ev;
    int srv_fd;
    int cli_fd;
    size_t out_pending;
    size_t cli_send_accum;
    size_t cli_recv_accum;
    bool want_write;
} libevent_conn;

struct libevent_worker {
    pthread_t thr;
    pthread_barrier_t *ready;
    pthread_barrier_t *start;
    pthread_barrier_t *finish;
    libevent_worker *all;
    unsigned int workers;
    unsigned int idx;
    unsigned int conns;
    unsigned int k;

    struct event_base *base;
    int async_fd;
    struct event *async_ev;
    _Atomic bool alive;

    _Atomic unsigned int churn_reqs;
    unsigned int churn_pending;
    unsigned int churn_seq;

    libevent_conn *conn;
    uint64_t msgs_target;
    uint64_t sent_msgs;
    uint64_t done_msgs;
};

static void libevent_eventfd_notify(int fd)
{
    for (eventfd_t val = 1; /**/; val = 1) {
        ssize_t res = write(fd, &val, sizeof(val));
        if (res >= 0) {
            return;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN) {
            return;
        }

        for (;;) {
            res = read(fd, &val, sizeof(val));
            if (res >= 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }
    }
}

static void libevent_conn_cb(evutil_socket_t fd, short what, void *arg);

static void libevent_conn_update_events(libevent_conn *c)
{
    short want = LIBEVENT_READ | (c->out_pending ? LIBEVENT_WRITE : 0) | EV_PERSIST;
    bool want_write = (want & LIBEVENT_WRITE) != 0;
    if (want_write == c->want_write) {
        return;
    }

    c->want_write = want_write;
    event_del(c->ev);
    event_assign(c->ev, c->w->base, c->srv_fd, want, libevent_conn_cb, c);
    event_add(c->ev, NULL);
}

static void libevent_conn_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;

    libevent_conn *c = arg;
    char buf[4096];

    if (what & LIBEVENT_READ) {
        for (;;) {
            ssize_t n = read(c->srv_fd, buf, sizeof(buf));
            if (n > 0) {
                c->out_pending += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    if (what & LIBEVENT_WRITE) {
        static const char out[4096] = { 0 };
        while (c->out_pending) {
            size_t todo = c->out_pending;
            if (todo > sizeof(out)) {
                todo = sizeof(out);
            }

            ssize_t n = write(c->srv_fd, out, todo);
            if (n > 0) {
                c->out_pending -= (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    libevent_conn_update_events(c);
}

static void libevent_conn_reopen(libevent_conn *c)
{
    int fds[2];
    if (socketpair_nb(fds) != 0) {
        abort();
    }

    event_del(c->ev);

    close(c->srv_fd);
    close(c->cli_fd);

    c->srv_fd = fds[0];
    c->cli_fd = fds[1];
    c->out_pending = 0;
    c->cli_send_accum = 0;
    c->cli_recv_accum = 0;
    c->want_write = false;

    event_assign(c->ev, c->w->base, c->srv_fd, LIBEVENT_READ | EV_PERSIST, libevent_conn_cb, c);
    event_add(c->ev, NULL);
}

static void libevent_async_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)what;

    libevent_worker *w = arg;
    eventfd_t val;
    for (;;) {
        ssize_t n = read(fd, &val, sizeof(val));
        if (n >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }

    w->churn_pending += atomic_exchange_explicit(&w->churn_reqs, 0, memory_order_acq_rel);
}

static void *libevent_workers_thread(void *ptr)
{
    libevent_worker *w = ptr;
    w->base = event_base_new();
    if (!w->base) {
        abort();
    }

    w->async_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (w->async_fd < 0) {
        abort();
    }

    atomic_init(&w->churn_reqs, 0);
    w->churn_pending = 0;
    w->churn_seq = 0;
    w->async_ev = event_new(w->base, w->async_fd, LIBEVENT_READ | EV_PERSIST, libevent_async_cb, w);
    event_add(w->async_ev, NULL);
    atomic_store_explicit(&w->alive, true, memory_order_release);

    w->conn = calloc(w->conns, sizeof(*w->conn));
    if (!w->conn) {
        abort();
    }

    for (unsigned int i = 0; i < w->conns; ++i) {
        int fds[2];
        if (socketpair_nb(fds) != 0) {
            abort();
        }

        libevent_conn *c = &w->conn[i];
        c->w = w;
        c->srv_fd = fds[0];
        c->cli_fd = fds[1];
        c->out_pending = 0;
        c->cli_send_accum = 0;
        c->cli_recv_accum = 0;
        c->want_write = false;
        c->ev = event_new(w->base, c->srv_fd, LIBEVENT_READ | EV_PERSIST, libevent_conn_cb, c);
        event_add(c->ev, NULL);
    }

    pthread_barrier_wait(w->ready);
    pthread_barrier_wait(w->start);

    uint64_t async_every = (uint64_t)w->k * (uint64_t)w->conns;
    uint64_t next_async = async_every ? async_every : UINT64_MAX;

    char in[MSG_SIZE];
    memset(in, 'x', sizeof(in));

    unsigned int seq = 0;
    while (w->done_msgs < w->msgs_target) {
        unsigned int idxs[IO_BATCH];
        unsigned int nidx = 0;

        for (; nidx < IO_BATCH && w->sent_msgs < w->msgs_target; ++nidx) {
            unsigned int idx = seq++ % w->conns;
            idxs[nidx] = idx;

            libevent_conn *c = &w->conn[idx];
            ssize_t n = write(c->cli_fd, in, sizeof(in));
            if (n > 0) {
                c->cli_send_accum += (size_t)n;
                while (c->cli_send_accum >= MSG_SIZE && w->sent_msgs < w->msgs_target) {
                    c->cli_send_accum -= MSG_SIZE;
                    ++w->sent_msgs;
                }
            }
        }

        for (int i = 0; i < 3; ++i) {
            event_base_loop(w->base, EVLOOP_NONBLOCK);
        }

        for (unsigned int i = 0; i < nidx; ++i) {
            unsigned int idx = idxs[i];
            libevent_conn *c = &w->conn[idx];
            for (;;) {
                char out[4096];
                ssize_t n = read(c->cli_fd, out, sizeof(out));
                if (n > 0) {
                    c->cli_recv_accum += (size_t)n;
                    while (c->cli_recv_accum >= MSG_SIZE && w->done_msgs < w->msgs_target) {
                        c->cli_recv_accum -= MSG_SIZE;
                        ++w->done_msgs;
                    }
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
        }

        while (w->churn_pending && w->sent_msgs == w->done_msgs) {
            unsigned int idx = w->churn_seq++ % w->conns;
            libevent_conn_reopen(&w->conn[idx]);
            --w->churn_pending;
        }

        if (w->done_msgs >= next_async && w->workers > 1) {
            unsigned int dst = (w->idx + 1) % w->workers;
            libevent_worker *t = &w->all[dst];
            if (atomic_load_explicit(&t->alive, memory_order_acquire)) {
                atomic_fetch_add_explicit(&t->churn_reqs, 1, memory_order_release);
                libevent_eventfd_notify(t->async_fd);
            }
            next_async += async_every;
        }
    }

    atomic_store_explicit(&w->alive, false, memory_order_release);
    pthread_barrier_wait(w->finish);

    for (unsigned int i = 0; i < w->conns; ++i) {
        libevent_conn *c = &w->conn[i];
        event_del(c->ev);
        event_free(c->ev);
        close(c->srv_fd);
        close(c->cli_fd);
    }
    free(w->conn);

    event_del(w->async_ev);
    event_free(w->async_ev);
    close(w->async_fd);
    event_base_free(w->base);
    return NULL;
}

static void bench_libevent_workers(unsigned int workers, unsigned int conns, unsigned int k)
{
    pthread_barrier_t ready;
    pthread_barrier_t start;
    pthread_barrier_t finish;
    if (pthread_barrier_init(&ready, NULL, workers + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&start, NULL, workers + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&finish, NULL, workers) != 0) {
        abort();
    }

    libevent_worker *w = calloc(workers, sizeof(*w));
    if (!w) {
        abort();
    }

    for (unsigned int i = 0; i < workers; ++i) {
        w[i].ready = &ready;
        w[i].start = &start;
        w[i].finish = &finish;
        w[i].all = w;
        w[i].workers = workers;
        w[i].idx = i;
        w[i].conns = conns;
        w[i].k = k;
        w[i].msgs_target = (uint64_t)conns * MSGS_PER_CONN;
        if (pthread_create(&w[i].thr, NULL, libevent_workers_thread, &w[i]) != 0) {
            abort();
        }
    }

    pthread_barrier_wait(&ready);

    uint64_t start_ns = get_time_ns();
    pthread_barrier_wait(&start);

    for (unsigned int i = 0; i < workers; ++i) {
        pthread_join(w[i].thr, NULL);
    }
    uint64_t end_ns = get_time_ns();

    pthread_barrier_destroy(&ready);
    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&finish);

    uint64_t ops = (uint64_t)workers * (uint64_t)conns * MSGS_PER_CONN;
    print_benchmark("workers", "libevent", end_ns - start_ns, ops);
    free(w);
}

// --- libuv ---
typedef struct libuv_worker libuv_worker;

typedef struct {
    libuv_worker *w;
    uv_poll_t *poll;
    int srv_fd;
    int cli_fd;
    size_t out_pending;
    size_t cli_send_accum;
    size_t cli_recv_accum;
    bool want_write;
} libuv_conn;

struct libuv_worker {
    pthread_t thr;
    pthread_barrier_t *ready;
    pthread_barrier_t *start;
    pthread_barrier_t *finish;
    libuv_worker *all;
    unsigned int workers;
    unsigned int idx;
    unsigned int conns;
    unsigned int k;

    uv_loop_t *loop;
    uv_async_t async;
    _Atomic bool alive;
    _Atomic unsigned int churn_reqs;
    unsigned int churn_pending;
    unsigned int churn_seq;

    libuv_conn *conn;
    uint64_t msgs_target;
    uint64_t sent_msgs;
    uint64_t done_msgs;
};

static void libuv_poll_close_cb(uv_handle_t *h)
{
    typedef struct {
        int fd;
    } libuv_poll_close_ctx;

    libuv_poll_close_ctx *ctx = h->data;
    if (ctx && ctx->fd >= 0) {
        close(ctx->fd);
    }
    free(ctx);
    free(h);
}

static void libuv_conn_cb(uv_poll_t *p, int status, int events)
{
    (void)status;

    libuv_conn *c = p->data;
    char buf[4096];

    if (events & UV_READABLE) {
        for (;;) {
            ssize_t n = read(c->srv_fd, buf, sizeof(buf));
            if (n > 0) {
                c->out_pending += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    if (events & UV_WRITABLE) {
        static const char out[4096] = { 0 };
        while (c->out_pending) {
            size_t todo = c->out_pending;
            if (todo > sizeof(out)) {
                todo = sizeof(out);
            }

            ssize_t n = write(c->srv_fd, out, todo);
            if (n > 0) {
                c->out_pending -= (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    int want = UV_READABLE | (c->out_pending ? UV_WRITABLE : 0);
    if (!uv_is_closing((uv_handle_t *)c->poll)) {
        if (!!(want & UV_WRITABLE) != c->want_write) {
            uv_poll_start(c->poll, want, libuv_conn_cb);
        }
    }
    c->want_write = c->out_pending != 0;
}

static void libuv_conn_reopen(libuv_worker *w, libuv_conn *c)
{
    typedef struct {
        int fd;
    } libuv_poll_close_ctx;

    int fds[2];
    if (socketpair_nb(fds) != 0) {
        abort();
    }

    uv_poll_t *old = c->poll;
    if (old) {
        uv_poll_stop(old);
        libuv_poll_close_ctx *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            abort();
        }
        ctx->fd = c->srv_fd;
        old->data = ctx;
        uv_close((uv_handle_t *)old, libuv_poll_close_cb);
    } else if (c->srv_fd >= 0) {
        close(c->srv_fd);
    }

    close(c->cli_fd);

    c->srv_fd = fds[0];
    c->cli_fd = fds[1];
    c->out_pending = 0;
    c->cli_send_accum = 0;
    c->cli_recv_accum = 0;
    c->want_write = false;

    uv_poll_t *p = malloc(sizeof(*p));
    if (!p) {
        abort();
    }
    if (uv_poll_init(w->loop, p, c->srv_fd) != 0) {
        abort();
    }
    p->data = c;
    c->poll = p;
    uv_poll_start(p, UV_READABLE, libuv_conn_cb);
}

static void libuv_async_cb(uv_async_t *a)
{
    libuv_worker *w = a->data;
    w->churn_pending += atomic_exchange_explicit(&w->churn_reqs, 0, memory_order_acq_rel);
}

static void *libuv_workers_thread(void *ptr)
{
    libuv_worker *w = ptr;
    w->loop = uv_loop_new();
    if (!w->loop) {
        abort();
    }

    w->conn = calloc(w->conns, sizeof(*w->conn));
    if (!w->conn) {
        abort();
    }

    atomic_init(&w->churn_reqs, 0);
    w->churn_pending = 0;
    w->churn_seq = 0;

    if (uv_async_init(w->loop, &w->async, libuv_async_cb) != 0) {
        abort();
    }
    w->async.data = w;
    atomic_store_explicit(&w->alive, true, memory_order_release);

    for (unsigned int i = 0; i < w->conns; ++i) {
        libuv_conn *c = &w->conn[i];
        c->w = w;
        c->poll = NULL;
        c->srv_fd = -1;
        c->cli_fd = -1;
        c->cli_send_accum = 0;
        c->cli_recv_accum = 0;
        libuv_conn_reopen(w, c);
    }

    pthread_barrier_wait(w->ready);
    pthread_barrier_wait(w->start);

    uint64_t async_every = (uint64_t)w->k * (uint64_t)w->conns;
    uint64_t next_async = async_every ? async_every : UINT64_MAX;

    char in[MSG_SIZE];
    memset(in, 'x', sizeof(in));

    unsigned int seq = 0;
    while (w->done_msgs < w->msgs_target) {
        unsigned int idxs[IO_BATCH];
        unsigned int nidx = 0;

        for (; nidx < IO_BATCH && w->sent_msgs < w->msgs_target; ++nidx) {
            unsigned int idx = seq++ % w->conns;
            idxs[nidx] = idx;

            libuv_conn *c = &w->conn[idx];
            ssize_t n = write(c->cli_fd, in, sizeof(in));
            if (n > 0) {
                c->cli_send_accum += (size_t)n;
                while (c->cli_send_accum >= MSG_SIZE && w->sent_msgs < w->msgs_target) {
                    c->cli_send_accum -= MSG_SIZE;
                    ++w->sent_msgs;
                }
            }
        }

        for (int i = 0; i < 3; ++i) {
            uv_run(w->loop, UV_RUN_NOWAIT);
        }

        for (unsigned int i = 0; i < nidx; ++i) {
            unsigned int idx = idxs[i];
            libuv_conn *c = &w->conn[idx];
            for (;;) {
                char out[4096];
                ssize_t n = read(c->cli_fd, out, sizeof(out));
                if (n > 0) {
                    c->cli_recv_accum += (size_t)n;
                    while (c->cli_recv_accum >= MSG_SIZE && w->done_msgs < w->msgs_target) {
                        c->cli_recv_accum -= MSG_SIZE;
                        ++w->done_msgs;
                    }
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
        }

        while (w->churn_pending && w->sent_msgs == w->done_msgs) {
            unsigned int idx = w->churn_seq++ % w->conns;
            libuv_conn_reopen(w, &w->conn[idx]);
            --w->churn_pending;
        }

        if (w->done_msgs >= next_async && w->workers > 1) {
            unsigned int dst = (w->idx + 1) % w->workers;
            libuv_worker *t = &w->all[dst];
            if (atomic_load_explicit(&t->alive, memory_order_acquire)) {
                atomic_fetch_add_explicit(&t->churn_reqs, 1, memory_order_release);
                uv_async_send(&t->async);
            }
            next_async += async_every;
        }
    }

    atomic_store_explicit(&w->alive, false, memory_order_release);
    pthread_barrier_wait(w->finish);

    uv_close((uv_handle_t *)&w->async, NULL);

    for (unsigned int i = 0; i < w->conns; ++i) {
        libuv_conn *c = &w->conn[i];
        if (c->poll && !uv_is_closing((uv_handle_t *)c->poll)) {
            uv_poll_stop(c->poll);
            typedef struct {
                int fd;
            } libuv_poll_close_ctx;
            libuv_poll_close_ctx *ctx = malloc(sizeof(*ctx));
            if (!ctx) {
                abort();
            }
            ctx->fd = c->srv_fd;
            c->poll->data = ctx;
            uv_close((uv_handle_t *)c->poll, libuv_poll_close_cb);
        } else if (c->srv_fd >= 0) {
            close(c->srv_fd);
        }
        if (c->cli_fd >= 0) {
            close(c->cli_fd);
        }
    }

    uv_run(w->loop, UV_RUN_DEFAULT);

    uv_loop_close(w->loop);
    free(w->loop);
    free(w->conn);
    return NULL;
}

static void bench_libuv_workers(unsigned int workers, unsigned int conns, unsigned int k)
{
    pthread_barrier_t ready;
    pthread_barrier_t start;
    pthread_barrier_t finish;
    if (pthread_barrier_init(&ready, NULL, workers + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&start, NULL, workers + 1) != 0) {
        abort();
    }
    if (pthread_barrier_init(&finish, NULL, workers) != 0) {
        abort();
    }

    libuv_worker *w = calloc(workers, sizeof(*w));
    if (!w) {
        abort();
    }

    for (unsigned int i = 0; i < workers; ++i) {
        w[i].ready = &ready;
        w[i].start = &start;
        w[i].finish = &finish;
        w[i].all = w;
        w[i].workers = workers;
        w[i].idx = i;
        w[i].conns = conns;
        w[i].k = k;
        w[i].msgs_target = (uint64_t)conns * MSGS_PER_CONN;
        if (pthread_create(&w[i].thr, NULL, libuv_workers_thread, &w[i]) != 0) {
            abort();
        }
    }

    pthread_barrier_wait(&ready);

    uint64_t start_ns = get_time_ns();
    pthread_barrier_wait(&start);

    for (unsigned int i = 0; i < workers; ++i) {
        pthread_join(w[i].thr, NULL);
    }
    uint64_t end_ns = get_time_ns();

    pthread_barrier_destroy(&ready);
    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&finish);

    uint64_t ops = (uint64_t)workers * (uint64_t)conns * MSGS_PER_CONN;
    print_benchmark("workers", "libuv", end_ns - start_ns, ops);
    free(w);
}

int main(void)
{
    print_versions();

    unsigned int workers = env_u32("EVIO_BENCH_WORKERS", DEF_WORKERS, MAX_WORKERS);
    unsigned int conns = env_u32("EVIO_BENCH_CONNS", DEF_CONNS, MAX_CONNS);
    unsigned int k = env_u32("EVIO_BENCH_K", DEF_K, MAX_K);

    if (workers == 0) {
        workers = 1;
    }

    conns = cap_conns_by_rlimit(workers, conns);
    if (conns == 0) {
        conns = 1;
    }

    bench_evio_workers(workers, conns, k, false);
    bench_evio_workers(workers, conns, k, true);
    bench_libev_workers(workers, conns, k);
    bench_libevent_workers(workers, conns, k);
    bench_libuv_workers(workers, conns, k);
    return EXIT_SUCCESS;
}
