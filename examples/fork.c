#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "evio.h"

#define PORT 8080
#define NUM_CHILDREN 2
#define BUFFER_SIZE 1024

// -- Child Process Logic --

typedef struct {
    evio_poll io;
    evio_cleanup cleanup;
    size_t length;
    size_t sent;
    char buffer[BUFFER_SIZE];
} client_info;

static void client_close(evio_loop *loop, client_info *client)
{
    evio_poll_stop(loop, &client->io);
    evio_cleanup_stop(loop, &client->cleanup);
    close(client->io.fd);
    free(client);
}

static void client_cleanup_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    client_close(loop, base->data);
}

// Callback to handle I/O on client connections
static void client_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    client_info *client = base->data;
    evio_poll *w = &client->io;

    if (!client->length) {
        if (!(emask & EVIO_READ)) {
            return;
        }
        ssize_t n = read(w->fd, client->buffer, sizeof(client->buffer));
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            return;
        }
        if (n <= 0) {
            goto disconnected;
        }
        client->length = (size_t)n;
    }

    ssize_t written = send(w->fd, client->buffer + client->sent,
                           client->length - client->sent, MSG_NOSIGNAL);
    if (written < 0 && (errno == EAGAIN || errno == EINTR)) {
        evio_poll_change(loop, w, w->fd, EVIO_WRITE);
        return;
    }
    if (written <= 0) {
        goto disconnected;
    }
    client->sent += (size_t)written;
    if (client->sent < client->length) {
        evio_poll_change(loop, w, w->fd, EVIO_WRITE);
        return;
    }

    client->length = 0;
    client->sent = 0;
    evio_poll_change(loop, w, w->fd, EVIO_READ);
    return;

disconnected:
    printf("[Child %d] Client disconnected (fd: %d).\n", getpid(), w->fd);
    client_close(loop, client);
}

// Callback to handle new connections on the listening socket
static void accept_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_poll *w = (evio_poll *)base;
    struct sockaddr_in client_addr = { 0 };
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept4(w->fd, (struct sockaddr *)&client_addr, &client_len, SOCK_NONBLOCK);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EINTR) {
            return;
        }
        perror("accept");
        return;
    }

    char client_ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip))) {
        perror("inet_ntop");
        close(client_fd);
        return;
    }

    printf("[Child %d] Accepted new connection (fd: %d) from %s:%d\n",
           getpid(), client_fd, client_ip, ntohs(client_addr.sin_port));

    client_info *client = malloc(sizeof(*client));
    if (!client) {
        close(client_fd);
        return;
    }
    client->length = 0;
    client->sent = 0;
    evio_poll_init(&client->io, client_cb, client_fd, EVIO_READ);
    client->io.data = client;
    evio_cleanup_init(&client->cleanup, client_cleanup_cb);
    client->cleanup.data = client;
    evio_cleanup_start(loop, &client->cleanup);
    evio_poll_start(loop, &client->io);
}

// Child's signal handler for graceful shutdown when signaled by the parent
static void child_signal_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    printf("[Child %d] Caught SIGTERM, shutting down.\n", getpid());
    evio_break(loop, EVIO_BREAK_ALL);
}

// Main function for child processes
static int run_child(int listen_fd, const sigset_t *parent_mask)
{
    printf("[Child %d] Starting up.\n", getpid());

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        perror("evio_loop_new in child");
        return EXIT_FAILURE;
    }

    // Watcher for new connections
    evio_poll listen_w;
    evio_poll_init(&listen_w, accept_cb, listen_fd, EVIO_READ);
    evio_poll_start(loop, &listen_w);

    // Watcher for shutdown signal from parent
    evio_signal signal_w;
    evio_signal_init(&signal_w, child_signal_cb, SIGTERM);
    evio_signal_start(loop, &signal_w);

    // Unblock signals now that handlers are set up
    pthread_sigmask(SIG_SETMASK, parent_mask, NULL);

    evio_run(loop, EVIO_RUN_DEFAULT);

    evio_loop_free(loop);
    printf("[Child %d] Shutting down.\n", getpid());
    return EXIT_SUCCESS;
}

// -- Parent Process Logic --

// A struct to hold info about a child process
typedef struct {
    pid_t pid;
    evio_poll pidfd_w;
} child_info;

// A struct for the parent's loop user data
typedef struct {
    child_info *children;
    int children_alive;
    int status;
} parent_data;

// Helper for pidfd_open since glibc might not wrap it
static int pidfd_open(pid_t pid, unsigned int flags)
{
    return syscall(__NR_pidfd_open, pid, flags);
}

static void parent_cleanup(evio_loop *loop, parent_data *pdata, int signum)
{
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        child_info *child = &pdata->children[i];
        if (child->pid <= 0) {
            continue;
        }

        int rc;
        if (child->pidfd_w.fd >= 0) {
            rc = syscall(__NR_pidfd_send_signal, child->pidfd_w.fd, signum, NULL, 0);
        } else {
            // No wait has run for a child without a pidfd.
            rc = kill(child->pid, signum);
        }
        if (rc < 0 && errno != ESRCH) {
            perror("signal child");
            pdata->status = EXIT_FAILURE;
        }
    }

    for (int i = 0; i < NUM_CHILDREN; ++i) {
        child_info *child = &pdata->children[i];
        if (child->pid > 0) {
            pid_t rc;
            do {
                rc = waitpid(child->pid, NULL, 0);
            } while (rc < 0 && errno == EINTR);
            if (rc < 0 && errno != ECHILD) {
                perror("waitpid");
                pdata->status = EXIT_FAILURE;
            }
            child->pid = 0;
        }
        if (child->pidfd_w.fd >= 0) {
            evio_poll_stop(loop, &child->pidfd_w);
            close(child->pidfd_w.fd);
            child->pidfd_w.fd = -1;
        }
    }
}

// Callback for when a child process exits (pidfd becomes readable)
static void child_exit_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_poll *w = (evio_poll *)base;
    parent_data *pdata = evio_get_userdata(loop);
    child_info *child = base->data;
    pid_t rc = waitpid(child->pid, NULL, WNOHANG);
    if (rc < 0) {
        perror("waitpid");
        pdata->status = EXIT_FAILURE;
        evio_break(loop, EVIO_BREAK_ALL);
        return;
    }
    if (!rc) {
        return;
    }

    printf("[Parent] Detected child exit (pidfd: %d).\n", w->fd);

    evio_poll_stop(loop, w);
    close(w->fd);
    w->fd = -1;
    child->pid = 0;

    pdata->children_alive--;
    if (pdata->children_alive == 0) {
        printf("[Parent] All children have terminated.\n");
        evio_break(loop, EVIO_BREAK_ALL);
    }
}

// Callback for when the parent process catches SIGINT
static void parent_signal_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    parent_data *pdata = evio_get_userdata(loop);
    printf("\n[Parent] Caught SIGINT, signaling children to terminate.\n");

    for (int i = 0; i < NUM_CHILDREN; ++i) {
        child_info *child = &pdata->children[i];
        if (child->pidfd_w.fd < 0) {
            continue;
        }
        if (syscall(__NR_pidfd_send_signal, child->pidfd_w.fd, SIGTERM, NULL, 0) < 0 &&
            errno != ESRCH) {
            perror("pidfd_send_signal");
            pdata->status = EXIT_FAILURE;
            evio_break(loop, EVIO_BREAK_ALL);
            return;
        }
    }

    // One-shot.
    evio_signal_stop(loop, (evio_signal *)base);
}

int main(int argc, char *argv[])
{
    int port = PORT;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        char *end = NULL;
        errno = 0;
        long value = strtol(argv[1], &end, 10);
        if (errno || end == argv[1] || *end || value <= 0 || value > 65535) {
            fprintf(stderr, "Invalid port number: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
        port = (int)value;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int));

    struct sockaddr_in serv_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port),
    };

    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("[Parent] Server listening on port %d\n", port);

    // -- Parent Setup --
    evio_loop *parent_loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!parent_loop) {
        perror("evio_loop_new in parent");
        close(listen_fd);
        return EXIT_FAILURE;
    }
    parent_data pdata = {
        .children = calloc(NUM_CHILDREN, sizeof(child_info)),
        .status = EXIT_SUCCESS,
    };
    if (!pdata.children) {
        perror("calloc");
        evio_loop_free(parent_loop);
        close(listen_fd);
        return EXIT_FAILURE;
    }
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        pdata.children[i].pidfd_w.fd = -1;
    }
    evio_set_userdata(parent_loop, &pdata);

    struct sigaction child_action = { .sa_handler = SIG_DFL };
    struct sigaction old_child_action;
    sigemptyset(&child_action.sa_mask);
    if (sigaction(SIGCHLD, &child_action, &old_child_action) < 0) {
        perror("sigaction");
        free(pdata.children);
        evio_loop_free(parent_loop);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    // Parent handles SIGINT to initiate shutdown
    evio_signal parent_sig_w;
    evio_signal_init(&parent_sig_w, parent_signal_cb, SIGINT);
    evio_signal_start(parent_loop, &parent_sig_w);

    // Block signals before forking to prevent races
    sigset_t mask;
    sigset_t oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, &oldmask);

    int shutdown_signal = SIGKILL;

    // Fork children and watch them with pidfd
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            pdata.status = EXIT_FAILURE;
            goto cleanup;
        }

        if (pid == 0) {
            // Child process: clean up parent resources and run its own logic
            evio_loop_free(parent_loop);
            for (int j = 0; j < NUM_CHILDREN; ++j) {
                if (pdata.children[j].pidfd_w.fd >= 0) {
                    close(pdata.children[j].pidfd_w.fd);
                }
            }
            free(pdata.children);
            int status = run_child(listen_fd, &oldmask);
            close(listen_fd);
            return status;
        }

        // Parent process: store child info and watch its pidfd
        pdata.children[i].pid = pid;
        int pfd = pidfd_open(pid, 0);
        if (pfd < 0) {
            perror("pidfd_open");
            pdata.status = EXIT_FAILURE;
            goto cleanup;
        }

        child_info *child = &pdata.children[i];
        evio_poll_init(&child->pidfd_w, child_exit_cb, pfd, EVIO_READ);
        child->pidfd_w.data = child;
        evio_poll_start(parent_loop, &child->pidfd_w);
        pdata.children_alive++;
    }

    close(listen_fd); // Parent doesn't need the listening socket anymore
    listen_fd = -1;

    // Unblock signals now that children are forked and watched
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);

    printf("[Parent] Forked %d children. Waiting for events... (Press Ctrl+C to stop)\n", NUM_CHILDREN);
    shutdown_signal = SIGTERM;
    evio_run(parent_loop, EVIO_RUN_DEFAULT);

cleanup:
    parent_cleanup(parent_loop, &pdata, shutdown_signal);
    free(pdata.children);
    evio_loop_free(parent_loop);
    if (sigaction(SIGCHLD, &old_child_action, NULL) < 0) {
        perror("sigaction");
        pdata.status = EXIT_FAILURE;
    }
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    if (pdata.status == EXIT_SUCCESS) {
        printf("[Parent] Event loop finished. Exiting.\n");
    }

    return pdata.status;
}
