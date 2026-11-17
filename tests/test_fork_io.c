#include "test.h"
#include "fork_wait.h"

static ssize_t fork_example_send(int fd, const void *buffer, size_t length, int flags);
static int fork_example_accept4(int fd, struct sockaddr *address, socklen_t *length, int flags);

static size_t fork_send_limit;
static int fork_accept_error;
static int fork_accept_last_error;
static size_t fork_accept_calls;

#define send fork_example_send
#define accept4 fork_example_accept4
#include "../examples/fork_io.h"
#undef send
#undef accept4

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
