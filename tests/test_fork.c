#include "test.h"
#include "fork_wait.h"

#include <stdarg.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int fork_example_main(int argc, char *argv[]);
static pid_t fork_example_spawn(void);
static long fork_example_syscall(long number, ...);
static int fork_example_signal(pid_t pid, int signum);
static void *fork_example_calloc(size_t count, size_t size);

#define main fork_example_main
#define fork fork_example_spawn
#define syscall fork_example_syscall
#define kill fork_example_signal
#define calloc fork_example_calloc
#include "../examples/fork.c"
#undef main
#undef fork
#undef syscall
#undef kill
#undef calloc

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
