#include <sys/wait.h>
#include <sys/syscall.h>

#include "fork_io.h"

#define PORT 8080
#define NUM_CHILDREN 2

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
    listener_info listener;
    evio_poll_init(&listener.io, accept_cb, listen_fd, EVIO_READ);
    listener.io.data = &listener;
    evio_timer_init(&listener.retry, accept_retry_cb, 0);
    listener.retry.data = &listener;
    evio_poll_start(loop, &listener.io);

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

static int signal_child(child_info *child, int signum)
{
    if (child->pidfd_w.fd >= 0 &&
        syscall(__NR_pidfd_send_signal, child->pidfd_w.fd, signum, NULL, 0) == 0) {
        return 0;
    }

    // The parent is the sole reaper; a live child's PID cannot be reused before waitpid.
    pid_t rc;
    do {
        rc = waitpid(child->pid, NULL, WNOHANG);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0) {
        return kill(child->pid, signum);
    }
    if (rc > 0 || errno == ECHILD) {
        child->pid = 0;
        return 0;
    }
    return -1;
}

static void parent_cleanup(evio_loop *loop, parent_data *pdata, int signum)
{
    int wait_options[NUM_CHILDREN] = { 0 };
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        child_info *child = &pdata->children[i];
        if (child->pid <= 0) {
            continue;
        }

        if (signal_child(child, signum) < 0) {
            perror("signal child");
            pdata->status = EXIT_FAILURE;
            wait_options[i] = WNOHANG;
        }
    }

    for (int i = 0; i < NUM_CHILDREN; ++i) {
        child_info *child = &pdata->children[i];
        if (child->pid > 0) {
            pid_t rc;
            do {
                rc = waitpid(child->pid, NULL, wait_options[i]);
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
    printf("\n[Parent] Caught SIGINT, signaling children to terminate.\n");
    evio_signal_stop(loop, (evio_signal *)base);
    evio_break(loop, EVIO_BREAK_ALL);
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
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(listen_fd);
        return EXIT_FAILURE;
    }

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
