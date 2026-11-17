#include "test.h"

#include <stdarg.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int fork_example_main(int argc, char *argv[]);
static pid_t fork_example_spawn(void);
static long fork_example_syscall(long number, ...);
static int fork_example_signal(pid_t pid, int signum);
static void *fork_example_calloc(size_t count, size_t size);
static ssize_t fork_example_send(int fd, const void *buffer, size_t length, int flags);
static int fork_example_accept4(int fd, struct sockaddr *address, socklen_t *length, int flags);

static size_t fork_send_limit;
static int fork_accept_error;
static int fork_accept_last_error;
static size_t fork_accept_calls;

#define main fork_example_main
#define fork fork_example_spawn
#define syscall fork_example_syscall
#define kill fork_example_signal
#define calloc fork_example_calloc
#define send fork_example_send
#define accept4 fork_example_accept4
#include "../examples/fork.c"
#undef main
#undef fork
#undef syscall
#undef kill
#undef calloc
#undef send
#undef accept4

typedef struct {
    atomic_int fork_calls;
    atomic_int pidfd_calls;
    atomic_int children[NUM_CHILDREN];
    atomic_int term_calls[NUM_CHILDREN];
    atomic_int kill_calls[NUM_CHILDREN];
    atomic_int raw_calls[NUM_CHILDREN];
    int fail_fork;
    int fail_pidfd;
    bool fail_calloc;
    bool deny_pidfd_signal;
    bool deny_kill;
    atomic_bool injected;
} fork_shared;

static_assert(ATOMIC_INT_LOCK_FREE == 2);
static_assert(ATOMIC_BOOL_LOCK_FREE == 2);

typedef struct {
    fork_shared *shared;
    struct sigaction old_action;
    int old_subreaper;
    pid_t parent;
    int parent_fd;
    int control[2];
    int children[NUM_CHILDREN];
    bool pidfd_unavailable;
} fork_fixture;

static fork_shared *fork_state;
static int fork_control;
static int fork_handles[NUM_CHILDREN];
static int fork_pollfds[NUM_CHILDREN];

static void *fork_example_calloc(size_t count, size_t size)
{
    if (fork_state->fail_calloc) {
        atomic_store(&fork_state->injected, true);
        errno = ENOMEM;
        return NULL;
    }
    return calloc(count, size);
}

static ssize_t fork_example_send(int fd, const void *buffer, size_t length, int flags)
{
    if (fork_send_limit && length > fork_send_limit) {
        length = fork_send_limit;
    }
    return send(fd, buffer, length, flags);
}

static int fork_example_accept4(int fd, struct sockaddr *address, socklen_t *length, int flags)
{
    ++fork_accept_calls;
    if (fork_accept_error) {
        errno = fork_accept_error;
        fork_accept_last_error = errno;
        return -1;
    }
    int client = accept4(fd, address, length, flags);
    fork_accept_last_error = client < 0 ? errno : 0;
    return client;
}

static pid_t fork_example_spawn(void)
{
    int index = atomic_fetch_add(&fork_state->fork_calls, 1);
    if (index + 1 == fork_state->fail_fork) {
        atomic_store(&fork_state->injected, true);
        errno = EAGAIN;
        return -1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(fork_control);
        fork_control = -1;
        for (int i = 0; i < NUM_CHILDREN; ++i) {
            if (fork_handles[i] >= 0) {
                close(fork_handles[i]);
                fork_handles[i] = -1;
            }
        }
        raise(SIGSTOP);
        return 0;
    }
    if (pid < 0) {
        return pid;
    }

    atomic_store(&fork_state->children[index], pid);
    int fd = syscall(__NR_pidfd_open, pid, 0);
    if (fd < 0) {
        _exit(125);
    }
    fork_handles[index] = fd;
    siginfo_t info;
    if (waitid(P_PID, pid, &info, WSTOPPED | WNOWAIT) < 0) {
        _exit(125);
    }

    struct iovec iov = { .iov_base = &index, .iov_len = sizeof(index) };
    union {
        struct cmsghdr align;
        char data[CMSG_SPACE(sizeof(fd))];
    } control = { .data = { 0 } };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control.data,
        .msg_controllen = sizeof(control.data),
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    if (sendmsg(fork_control, &msg, 0) != sizeof(index)) {
        _exit(125);
    }
    return pid;
}

static long fork_example_syscall(long number, ...)
{
    va_list args;
    va_start(args, number);
    if (number == __NR_pidfd_open) {
        pid_t pid = va_arg(args, pid_t);
        unsigned int flags = va_arg(args, unsigned int);
        va_end(args);
        int call = atomic_fetch_add(&fork_state->pidfd_calls, 1) + 1;
        if (call == fork_state->fail_pidfd) {
            atomic_store(&fork_state->injected, true);
            errno = EMFILE;
            return -1;
        }
        long fd = syscall(number, pid, flags);
        for (int i = 0; i < NUM_CHILDREN; ++i) {
            if (atomic_load(&fork_state->children[i]) == pid) {
                fork_pollfds[i] = fd;
                break;
            }
        }
        return fd;
    }

    if (number != __NR_pidfd_send_signal) {
        va_end(args);
        _exit(125);
    }
    int fd = va_arg(args, int);
    int signum = va_arg(args, int);
    void *info = va_arg(args, void *);
    int flags = va_arg(args, int);
    va_end(args);
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        if (fork_pollfds[i] == fd) {
            if (signum == SIGTERM) {
                atomic_fetch_add(&fork_state->term_calls[i], 1);
            } else if (signum == SIGKILL) {
                atomic_fetch_add(&fork_state->kill_calls[i], 1);
            }
            if (fork_state->deny_pidfd_signal) {
                errno = EPERM;
                return -1;
            }
            return syscall(number, fd, signum, info, flags);
        }
    }
    _exit(125);
}

static int fork_example_signal(pid_t pid, int signum)
{
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        if (atomic_load(&fork_state->children[i]) == pid) {
            atomic_fetch_add(&fork_state->raw_calls[i], 1);
            if (signum == SIGKILL) {
                atomic_fetch_add(&fork_state->kill_calls[i], 1);
            }
            if (fork_state->deny_kill) {
                errno = EPERM;
                return -1;
            }
            // A stale numeric PID must never reach kill() during regression checks.
            return syscall(__NR_pidfd_send_signal, fork_handles[i], signum, NULL, 0);
        }
    }
    _exit(125);
}

static int fork_setup(void **state)
{
    fork_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    *state = f;
    f->parent_fd = -1;
    f->control[0] = f->control[1] = -1;
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        f->children[i] = -1;
    }
    f->shared = mmap(NULL, sizeof(*f->shared), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert_ptr_not_equal(f->shared, MAP_FAILED);
    atomic_init(&f->shared->fork_calls, 0);
    atomic_init(&f->shared->pidfd_calls, 0);
    atomic_init(&f->shared->injected, false);
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        atomic_init(&f->shared->children[i], 0);
        atomic_init(&f->shared->term_calls[i], 0);
        atomic_init(&f->shared->kill_calls[i], 0);
        atomic_init(&f->shared->raw_calls[i], 0);
    }
    assert_int_equal(prctl(PR_GET_CHILD_SUBREAPER, &f->old_subreaper), 0);
    assert_int_equal(prctl(PR_SET_CHILD_SUBREAPER, 1), 0);
    struct sigaction action = { .sa_handler = SIG_DFL };
    sigemptyset(&action.sa_mask);
    assert_int_equal(sigaction(SIGCHLD, &action, &f->old_action), 0);
    int fd = syscall(__NR_pidfd_open, getpid(), 0);
    if (fd < 0) {
        assert_int_equal(errno, ENOSYS);
        f->pidfd_unavailable = true;
        return 0;
    }
    int rc = syscall(__NR_pidfd_send_signal, fd, 0, NULL, 0);
    int err = errno;
    close(fd);
    if (rc < 0) {
        assert_int_equal(err, ENOSYS);
        f->pidfd_unavailable = true;
    }
    return 0;
}

static int fork_teardown(void **state)
{
    fork_fixture *f = *state;
    if (f->parent > 0 && waitpid(f->parent, NULL, WNOHANG) == 0) {
        kill(f->parent, SIGKILL);
        while (waitpid(f->parent, NULL, 0) < 0 && errno == EINTR) {}
    }
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        pid_t pid = atomic_load(&f->shared->children[i]);
        if (pid > 0 && waitpid(pid, NULL, WNOHANG) == 0) {
            kill(pid, SIGKILL);
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        }
        if (f->children[i] >= 0) {
            close(f->children[i]);
        }
    }
    if (f->parent_fd >= 0) {
        close(f->parent_fd);
    }
    for (int i = 0; i < 2; ++i) {
        if (f->control[i] >= 0) {
            close(f->control[i]);
        }
    }
    sigaction(SIGCHLD, &f->old_action, NULL);
    prctl(PR_SET_CHILD_SUBREAPER, f->old_subreaper);
    munmap(f->shared, sizeof(*f->shared));
    free(f);
    return 0;
}

static void fork_wait_event(int fd, short events)
{
    struct pollfd pfd = { .fd = fd, .events = events };
    assert_int_equal(poll(&pfd, 1, 5000), 1);
    if (events) {
        assert_true(pfd.revents & events);
    } else {
        assert_true(pfd.revents & POLLHUP);
    }
}

static void fork_start(fork_fixture *f)
{
    assert_int_equal(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, f->control), 0);
    f->parent = fork();
    assert_true(f->parent >= 0);
    if (f->parent == 0) {
        close(f->control[0]);
        fork_control = f->control[1];
        fork_state = f->shared;
        for (int i = 0; i < NUM_CHILDREN; ++i) {
            fork_handles[i] = fork_pollfds[i] = -1;
        }
        int fd = open("/dev/null", O_WRONLY);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
            _exit(125);
        }
        close(fd);
        struct sigaction action = { .sa_handler = SIG_IGN };
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGCHLD, &action, NULL) < 0) {
            _exit(125);
        }
        char name[] = "example-fork";
        char *argv[] = { name, NULL };
        int status = fork_example_main(1, argv);
        if (fork_control >= 0) {
            if (sigaction(SIGCHLD, NULL, &action) < 0 || action.sa_handler != SIG_IGN) {
                _exit(125);
            }
            // LeakSanitizer must be able to wait for its own tracer.
            action.sa_handler = SIG_DFL;
            action.sa_flags = 0;
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGCHLD, &action, NULL) < 0) {
                _exit(125);
            }
        }
        for (int i = 0; i < NUM_CHILDREN; ++i) {
            if (fork_handles[i] >= 0) {
                close(fork_handles[i]);
            }
        }
        if (fork_control >= 0) {
            close(fork_control);
        }
        exit(status);
    }
    close(f->control[1]);
    f->control[1] = -1;
    f->parent_fd = syscall(__NR_pidfd_open, f->parent, 0);
    assert_true(f->parent_fd >= 0);
}

static int fork_receive_child(fork_fixture *f)
{
    int index = -1;
    int fd;
    struct iovec iov = { .iov_base = &index, .iov_len = sizeof(index) };
    union {
        struct cmsghdr align;
        // Exclude trailing padding so the kernel installs at most one fd.
        char data[CMSG_LEN(sizeof(fd))];
    } control = { .data = { 0 } };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control.data,
        .msg_controllen = sizeof(control.data),
    };
    ssize_t n = recvmsg(f->control[0], &msg, MSG_CMSG_CLOEXEC);
    if (n < 0) {
        return errno;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(fd))) {
        return EPROTO;
    }

    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    if (n != sizeof(index) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        index < 0 || index >= NUM_CHILDREN || f->children[index] >= 0) {
        close(fd);
        return EPROTO;
    }

    f->children[index] = fd;
    return 0;
}

static void fork_receive(fork_fixture *f, int count)
{
    for (int i = 0; i < count; ++i) {
        fork_wait_event(f->control[0], POLLIN);
        assert_int_equal(fork_receive_child(f), 0);
    }
}

static void fork_wait_parent(fork_fixture *f, int expected)
{
    fork_wait_event(f->parent_fd, POLLIN);
    int status;
    assert_int_equal(waitpid(f->parent, &status, 0), f->parent);
    f->parent = 0;
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), expected);
}

static void fork_finish(fork_fixture *f, int expected)
{
    fork_wait_parent(f, expected);
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        pid_t pid = atomic_load(&f->shared->children[i]);
        if (pid > 0) {
            errno = 0;
            assert_int_equal(waitpid(pid, NULL, WNOHANG), -1);
            assert_int_equal(errno, ECHILD);
        }
    }
}

TEST(test_evio_fork_reaped_child_signal, .setup_func = fork_setup, .teardown_func = fork_teardown)
{
    fork_fixture *f = *state;
    if (f->pidfd_unavailable) {
        TEST_SKIPF("pidfd syscalls unavailable");
    }
    fork_start(f);
    fork_receive(f, NUM_CHILDREN);
    assert_int_equal(syscall(__NR_pidfd_send_signal, f->children[0], SIGTERM, NULL, 0), 0);
    assert_int_equal(syscall(__NR_pidfd_send_signal, f->children[0], SIGCONT, NULL, 0), 0);
    fork_wait_event(f->children[0], 0);
    assert_int_equal(syscall(__NR_pidfd_send_signal, f->parent_fd, SIGINT, NULL, 0), 0);
    assert_int_equal(syscall(__NR_pidfd_send_signal, f->children[1], SIGCONT, NULL, 0), 0);
    fork_finish(f, EXIT_SUCCESS);
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        assert_int_equal(atomic_load(&f->shared->raw_calls[i]), 0);
        assert_int_equal(atomic_load(&f->shared->kill_calls[i]), 0);
    }
    assert_int_equal(atomic_load(&f->shared->term_calls[0]), 0);
    assert_int_equal(atomic_load(&f->shared->term_calls[1]), 1);
}

static void fork_shutdown_denied(fork_fixture *f, bool deny_kill)
{
    if (f->pidfd_unavailable) {
        TEST_SKIPF("pidfd syscalls unavailable");
    }
    f->shared->deny_pidfd_signal = true;
    f->shared->deny_kill = deny_kill;
    fork_start(f);
    fork_receive(f, NUM_CHILDREN);
    assert_int_equal(syscall(__NR_pidfd_send_signal, f->parent_fd, SIGINT, NULL, 0), 0);
    if (deny_kill) {
        fork_wait_parent(f, EXIT_FAILURE);
    } else {
        for (int i = 0; i < NUM_CHILDREN; ++i) {
            assert_int_equal(syscall(__NR_pidfd_send_signal, f->children[i], SIGCONT, NULL, 0), 0);
        }
        fork_finish(f, EXIT_SUCCESS);
    }
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        assert_int_equal(atomic_load(&f->shared->term_calls[i]), 1);
        assert_int_equal(atomic_load(&f->shared->raw_calls[i]), 1);
    }
}

TEST(test_evio_fork_pidfd_signal_denied, .setup_func = fork_setup, .teardown_func = fork_teardown)
{
    fork_shutdown_denied(*state, false);
}

TEST(test_evio_fork_both_signals_denied, .setup_func = fork_setup, .teardown_func = fork_teardown)
{
    fork_shutdown_denied(*state, true);
}

static void fork_failed_start(fork_fixture *f, int children)
{
    if (f->pidfd_unavailable) {
        TEST_SKIPF("pidfd syscalls unavailable");
    }
    fork_start(f);
    fork_receive(f, children);
    fork_finish(f, EXIT_FAILURE);
    assert_true(atomic_load(&f->shared->injected));
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        assert_int_equal(atomic_load(&f->shared->children[i]) > 0, i < children);
        assert_int_equal(atomic_load(&f->shared->kill_calls[i]), i < children);
        assert_int_equal(atomic_load(&f->shared->term_calls[i]), 0);
    }
}

TEST(test_evio_fork_first_fork_failure, .setup_func = fork_setup, .teardown_func = fork_teardown)
{
    fork_fixture *f = *state;
    f->shared->fail_fork = 1;
    fork_failed_start(f, 0);
}

TEST(test_evio_fork_second_fork_failure, .setup_func = fork_setup, .teardown_func = fork_teardown)
{
    fork_fixture *f = *state;
    f->shared->fail_fork = 2;
    fork_failed_start(f, 1);
}

TEST(test_evio_fork_first_pidfd_failure, .setup_func = fork_setup, .teardown_func = fork_teardown)
{
    fork_fixture *f = *state;
    f->shared->fail_pidfd = 1;
    fork_failed_start(f, 1);
}

TEST(test_evio_fork_second_pidfd_failure, .setup_func = fork_setup, .teardown_func = fork_teardown)
{
    fork_fixture *f = *state;
    f->shared->fail_pidfd = 2;
    fork_failed_start(f, 2);
}

TEST(test_evio_fork_calloc_failure, .setup_func = fork_setup, .teardown_func = fork_teardown)
{
    fork_fixture *f = *state;
    f->shared->fail_calloc = true;
    fork_failed_start(f, 0);
}

typedef struct {
    evio_loop *loop;
    client_info *client;
    evio_poll ready_w;
    int ready_called;
    evio_signal signal_w;
    sigset_t oldmask;
    int sockets[2];
    int ready[2];
} fork_client_fixture;

static int fork_client_setup(void **state)
{
    fork_client_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    *state = f;
    f->sockets[0] = f->sockets[1] = -1;
    f->ready[0] = f->ready[1] = -1;
    assert_int_equal(pthread_sigmask(SIG_SETMASK, NULL, &f->oldmask), 0);
    f->loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(f->loop);
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, f->sockets), 0);
    f->client = calloc(1, sizeof(*f->client));
    assert_non_null(f->client);
    evio_poll_init(&f->client->io, client_cb, f->sockets[0], EVIO_READ);
    f->client->io.data = f->client;
    evio_cleanup_init(&f->client->cleanup, client_cleanup_cb);
    f->client->cleanup.data = f->client;
    evio_cleanup_start(f->loop, &f->client->cleanup);
    evio_poll_start(f->loop, &f->client->io);
    return 0;
}

static int fork_client_teardown(void **state)
{
    fork_client_fixture *f = *state;
    fork_send_limit = 0;
    if (f->loop) {
        evio_loop_free(f->loop);
    }
    if (f->sockets[1] >= 0) {
        close(f->sockets[1]);
    }
    for (int i = 0; i < 2; ++i) {
        if (f->ready[i] >= 0) {
            close(f->ready[i]);
        }
    }
    pthread_sigmask(SIG_SETMASK, &f->oldmask, NULL);
    free(f);
    return 0;
}

static void fork_client_receive(int fd, char *buffer, size_t length)
{
    size_t received = 0;
    while (received < length) {
        fork_wait_event(fd, POLLIN);
        ssize_t n = read(fd, buffer + received, length - received);
        assert_true(n > 0);
        received += (size_t)n;
    }
}

static size_t fork_client_fill(fork_client_fixture *f)
{
    int size = BUFFER_SIZE;
    assert_int_equal(setsockopt(f->sockets[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
    char buffer[BUFFER_SIZE] = { 0 };
    size_t queued = 0;
    for (int i = 0; i < 64; ++i) {
        ssize_t n = send(f->sockets[0], buffer, sizeof(buffer), MSG_NOSIGNAL);
        if (n < 0) {
            assert_int_equal(errno, EAGAIN);
            return queued;
        }
        assert_true(n > 0);
        queued += (size_t)n;
    }
    fail_msg("socket send buffer did not fill");
    return 0;
}

static void fork_client_drain(fork_client_fixture *f, size_t queued)
{
    char buffer[BUFFER_SIZE];
    while (queued) {
        size_t length = queued < sizeof(buffer) ? queued : sizeof(buffer);
        fork_client_receive(f->sockets[1], buffer, length);
        queued -= length;
    }
}

typedef struct {
    evio_loop *loop;
    listener_info listener;
    evio_signal signal_w;
    sigset_t oldmask;
    int peers[2];
} fork_listener_fixture;

static void fork_listener_connect(fork_listener_fixture *f, int index)
{
    int fd = f->listener.io.fd;
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    assert_int_equal(getsockname(fd, (struct sockaddr *)&address, &length), 0);
    f->peers[index] = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    assert_true(f->peers[index] >= 0);
    int rc = connect(f->peers[index], (struct sockaddr *)&address, length);
    assert_true(rc == 0 || (rc < 0 && errno == EINPROGRESS));
    fork_wait_event(fd, POLLIN);
}

static int fork_listener_setup(void **state)
{
    fork_listener_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    *state = f;
    f->listener.io.fd = -1;
    f->peers[0] = f->peers[1] = -1;
    fork_accept_error = 0;
    fork_accept_last_error = 0;
    fork_accept_calls = 0;
    assert_int_equal(pthread_sigmask(SIG_SETMASK, NULL, &f->oldmask), 0);
    f->loop = evio_loop_new(EVIO_FLAG_NONE);
    assert_non_null(f->loop);
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    assert_true(fd >= 0);
    f->listener.io.fd = fd;
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    assert_int_equal(bind(fd, (struct sockaddr *)&address, sizeof(address)), 0);
    assert_int_equal(listen(fd, 2), 0);
    evio_poll_init(&f->listener.io, accept_cb, fd, EVIO_READ);
    f->listener.io.data = &f->listener;
    evio_timer_init(&f->listener.retry, accept_retry_cb, 0);
    f->listener.retry.data = &f->listener;
    evio_poll_start(f->loop, &f->listener.io);
    evio_poll_update(f->loop);
    fork_listener_connect(f, 0);
    return 0;
}

static int fork_listener_teardown(void **state)
{
    fork_listener_fixture *f = *state;
    fork_accept_error = 0;
    fork_accept_last_error = 0;
    fork_accept_calls = 0;
    if (f->loop) {
        evio_loop_free(f->loop);
    }
    if (f->listener.io.fd >= 0) {
        close(f->listener.io.fd);
    }
    for (int i = 0; i < 2; ++i) {
        if (f->peers[i] >= 0) {
            close(f->peers[i]);
        }
    }
    pthread_sigmask(SIG_SETMASK, &f->oldmask, NULL);
    free(f);
    return 0;
}

static void fork_listener_retry(fork_listener_fixture *f)
{
    assert_true(f->listener.retry.active);
    f->loop->time = f->loop->timer.ptr[f->listener.retry.active - 1].time;
    evio_timer_update(f->loop);
    evio_invoke_pending(f->loop);
    evio_poll_update(f->loop);
    evio_poll_wait(f->loop, 0);
    evio_invoke_pending(f->loop);
}

TEST(test_evio_fork_nonblocking_accept, .setup_func = fork_listener_setup,
     .teardown_func = fork_listener_teardown)
{
    fork_listener_fixture *f = *state;
    accept_cb(f->loop, &f->listener.io.base, EVIO_READ);
    assert_int_equal(f->loop->cleanup.count, 1);
    client_info *client = f->loop->cleanup.ptr[0]->data;
    int client_fd = client->io.fd;
    int flags = fcntl(client_fd, F_GETFL);
    assert_true(flags >= 0);
    assert_true(flags & O_NONBLOCK);
    accept_cb(f->loop, &f->listener.io.base, EVIO_READ);
    assert_int_equal(f->loop->cleanup.count, 1);
    assert_true(f->listener.io.active);
    assert_false(f->listener.retry.active);
    evio_loop_free(f->loop);
    f->loop = NULL;
    errno = 0;
    assert_int_equal(fcntl(client_fd, F_GETFD), -1);
    assert_int_equal(errno, EBADF);
}

TEST(test_evio_fork_accept_emfile, .setup_func = fork_listener_setup,
     .teardown_func = fork_listener_teardown)
{
    fork_listener_fixture *f = *state;
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    assert_int_equal(f->loop->cleanup.count, 1);
    fork_listener_connect(f, 1);
    assert_int_equal(write(f->peers[0], "active", 6), 6);
    fork_accept_error = EMFILE;
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    evio_poll_wait(f->loop, 0);
    evio_invoke_pending(f->loop);
    fork_accept_error = 0;

    assert_int_equal(fork_accept_last_error, EMFILE);
    assert_int_equal(fork_accept_calls, 2);
    assert_false(f->listener.io.active);
    assert_true(f->listener.retry.active);
    assert_int_equal(evio_timer_remaining(f->loop, &f->listener.retry), EVIO_TIME_FROM_MSEC(100));
    assert_int_equal(f->loop->cleanup.count, 1);
    char buffer[6];
    fork_client_receive(f->peers[0], buffer, sizeof(buffer));
    assert_memory_equal(buffer, "active", sizeof(buffer));

    fork_listener_retry(f);
    assert_int_equal(fork_accept_calls, 3);
    assert_int_equal(fork_accept_last_error, 0);
    assert_true(f->listener.io.active);
    assert_false(f->listener.retry.active);
    assert_int_equal(f->loop->cleanup.count, 2);
    assert_int_equal(write(f->peers[1], "resume", 6), 6);
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    fork_client_receive(f->peers[1], buffer, sizeof(buffer));
    assert_memory_equal(buffer, "resume", sizeof(buffer));
}

TEST(test_evio_fork_accept_enfile, .setup_func = fork_listener_setup,
     .teardown_func = fork_listener_teardown)
{
    fork_listener_fixture *f = *state;
    fork_accept_error = ENFILE;
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    assert_int_equal(fork_accept_calls, 1);
    assert_int_equal(fork_accept_last_error, ENFILE);
    assert_false(f->listener.io.active);
    assert_true(f->listener.retry.active);
    assert_int_equal(evio_timer_remaining(f->loop, &f->listener.retry), EVIO_TIME_FROM_MSEC(100));
    assert_int_equal(f->loop->cleanup.count, 0);

    f->loop->time += EVIO_TIME_FROM_MSEC(100) - 1;
    evio_timer_update(f->loop);
    evio_invoke_pending(f->loop);
    evio_poll_wait(f->loop, 0);
    evio_invoke_pending(f->loop);
    assert_int_equal(fork_accept_calls, 1);
    assert_false(f->listener.io.active);
    assert_true(f->listener.retry.active);

    fork_listener_retry(f);
    assert_int_equal(fork_accept_calls, 2);
    assert_false(f->listener.io.active);
    assert_true(f->listener.retry.active);
    assert_int_equal(evio_timer_remaining(f->loop, &f->listener.retry), EVIO_TIME_FROM_MSEC(100));
    assert_int_equal(f->loop->cleanup.count, 0);

    fork_accept_error = 0;
    fork_listener_retry(f);
    assert_int_equal(fork_accept_calls, 3);
    assert_true(f->listener.io.active);
    assert_false(f->listener.retry.active);
    assert_int_equal(f->loop->cleanup.count, 1);
}

TEST(test_evio_fork_accept_sigterm, .setup_func = fork_listener_setup,
     .teardown_func = fork_listener_teardown)
{
    fork_listener_fixture *f = *state;
    fork_accept_error = ENFILE;
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    assert_true(f->listener.retry.active);
    f->loop->timer.ptr[f->listener.retry.active - 1].time = EVIO_TIME_MAX;
    evio_signal_init(&f->signal_w, child_signal_cb, SIGTERM);
    evio_signal_start(f->loop, &f->signal_w);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    assert_int_equal(pthread_sigmask(SIG_UNBLOCK, &mask, NULL), 0);
    assert_int_equal(raise(SIGTERM), 0);
    evio_run(f->loop, EVIO_RUN_DEFAULT);
    assert_int_equal(fork_accept_calls, 1);
    assert_false(f->listener.io.active);
    assert_true(f->listener.retry.active);
    evio_loop_free(f->loop);
    f->loop = NULL;
}

TEST(test_evio_fork_client_short_write, .setup_func = fork_client_setup,
     .teardown_func = fork_client_teardown)
{
    fork_client_fixture *f = *state;
    fork_send_limit = 3;
    assert_int_equal(write(f->sockets[1], "abcdef", 6), 6);
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    assert_int_equal(f->client->sent, 3);
    assert_int_equal(evio_poll_get_events(&f->client->io), EVIO_WRITE);
    assert_int_equal(write(f->sockets[1], "ghi", 3), 3);
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    assert_int_equal(f->client->length, 0);
    assert_int_equal(evio_poll_get_events(&f->client->io), EVIO_READ);
    char buffer[9];
    assert_int_equal(recv(f->sockets[0], buffer, 3, MSG_PEEK), 3);
    assert_memory_equal(buffer, "ghi", 3);
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    fork_client_receive(f->sockets[1], buffer, sizeof(buffer));
    assert_memory_equal(buffer, "abcdefghi", sizeof(buffer));
}

static void fork_client_ready_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    int *called = base->data;
    ++*called;
}

TEST(test_evio_fork_client_backpressure, .setup_func = fork_client_setup,
     .teardown_func = fork_client_teardown)
{
    fork_client_fixture *f = *state;
    size_t queued = fork_client_fill(f);
    assert_true(queued > 0);
    assert_int_equal(write(f->sockets[1], "first", 5), 5);
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    assert_int_equal(f->client->length, 5);
    assert_int_equal(f->client->sent, 0);
    assert_int_equal(evio_poll_get_events(&f->client->io), EVIO_WRITE);
    assert_int_equal(write(f->sockets[1], "second", 6), 6);
    assert_int_equal(pipe2(f->ready, O_NONBLOCK), 0);
    evio_poll_init(&f->ready_w, fork_client_ready_cb, f->ready[0], EVIO_READ);
    f->ready_w.data = &f->ready_called;
    evio_poll_start(f->loop, &f->ready_w);
    assert_int_equal(write(f->ready[1], "K", 1), 1);
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    assert_int_equal(f->ready_called, 1);
    char buffer[11];
    assert_int_equal(recv(f->sockets[0], buffer, 6, MSG_PEEK), 6);
    assert_memory_equal(buffer, "second", 6);
    evio_poll_stop(f->loop, &f->ready_w);
    fork_client_drain(f, queued);
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    fork_client_receive(f->sockets[1], buffer, sizeof(buffer));
    assert_memory_equal(buffer, "firstsecond", sizeof(buffer));
}

TEST(test_evio_fork_client_eof, .setup_func = fork_client_setup, .teardown_func = fork_client_teardown)
{
    fork_client_fixture *f = *state;
    assert_int_equal(shutdown(f->sockets[1], SHUT_WR), 0);
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    assert_int_equal(f->loop->cleanup.count, 0);
    errno = 0;
    assert_int_equal(fcntl(f->sockets[0], F_GETFD), -1);
    assert_int_equal(errno, EBADF);
}

TEST(test_evio_fork_client_sigterm, .setup_func = fork_client_setup, .teardown_func = fork_client_teardown)
{
    fork_client_fixture *f = *state;
    size_t queued = fork_client_fill(f);
    assert_true(queued > 0);
    assert_int_equal(write(f->sockets[1], "pending", 7), 7);
    evio_run(f->loop, EVIO_RUN_NOWAIT);
    assert_int_equal(f->client->length, 7);
    evio_signal_init(&f->signal_w, child_signal_cb, SIGTERM);
    evio_signal_start(f->loop, &f->signal_w);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    assert_int_equal(pthread_sigmask(SIG_UNBLOCK, &mask, NULL), 0);
    assert_int_equal(raise(SIGTERM), 0);
    evio_run(f->loop, EVIO_RUN_DEFAULT);
    evio_loop_free(f->loop);
    f->loop = NULL;
    errno = 0;
    assert_int_equal(fcntl(f->sockets[0], F_GETFD), -1);
    assert_int_equal(errno, EBADF);
    fork_client_drain(f, queued);
    char buffer;
    assert_int_equal(read(f->sockets[1], &buffer, 1), 0);
}
