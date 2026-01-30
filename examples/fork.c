#include <stdlib.h>
#include <stdio.h>
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

// Callback to handle I/O on client connections
static void client_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_poll *w = (evio_poll *)base;
    char buffer[BUFFER_SIZE];

    if (emask & EVIO_READ) {
        ssize_t n = read(w->fd, buffer, sizeof(buffer));
        if (n > 0) {
            // Echo data back to the client
            write(w->fd, buffer, n);
        } else {
            // Error or connection closed
            printf("[Child %d] Client disconnected (fd: %d).\n", getpid(), w->fd);
            evio_poll_stop(loop, w);
            close(w->fd);
            free(w); // Free the watcher allocated in accept_cb
        }
    }
}

// Callback to handle new connections on the listening socket
static void accept_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_poll *w = (evio_poll *)base;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(w->fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("accept");
        return;
    }

    printf("[Child %d] Accepted new connection (fd: %d) from %s:%d\n",
           getpid(), client_fd, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    // Create a new poll watcher for the client connection
    evio_poll *client_w = malloc(sizeof(evio_poll));
    if (!client_w) {
        close(client_fd);
        return;
    }
    evio_poll_init(client_w, client_cb, client_fd, EVIO_READ);
    evio_poll_start(loop, client_w);
}

// Child's signal handler for graceful shutdown when signaled by the parent
static void child_signal_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    printf("[Child %d] Caught SIGTERM, shutting down.\n", getpid());
    evio_break(loop, EVIO_BREAK_ALL);
}

// Main function for child processes
static void run_child(int listen_fd, const sigset_t *parent_mask)
{
    printf("[Child %d] Starting up.\n", getpid());

    evio_loop *loop = evio_loop_new(EVIO_FLAG_NONE);
    if (!loop) {
        perror("evio_loop_new in child");
        exit(EXIT_FAILURE);
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
    exit(EXIT_SUCCESS);
}

// -- Parent Process Logic --

// A struct to hold info about a child process
typedef struct {
    pid_t pid;
    evio_poll *pidfd_w;
} child_info;

// A struct for the parent's loop user data
typedef struct {
    child_info *children;
    int children_alive;
} parent_data;

// Helper for pidfd_open since glibc might not wrap it
static int pidfd_open(pid_t pid, unsigned int flags)
{
    return syscall(__NR_pidfd_open, pid, flags);
}

// Callback for when a child process exits (pidfd becomes readable)
static void child_exit_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    evio_poll *w = (evio_poll *)base;
    parent_data *pdata = evio_get_userdata(loop);

    printf("[Parent] Detected child exit (pidfd: %d).\n", w->fd);

    // Reap the child process using waitid with the pidfd *before* closing it.
    waitid(P_PIDFD, w->fd, NULL, WEXITED | WNOHANG);

    // Stop watching this pidfd, close it, and free the watcher memory.
    evio_poll_stop(loop, w);
    close(w->fd);
    free(w);

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
        if (pdata->children[i].pid > 0) {
            kill(pdata->children[i].pid, SIGTERM);
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
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
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
    parent_data pdata = {
        .children = calloc(NUM_CHILDREN, sizeof(child_info)),
        .children_alive = 0
    };
    evio_set_userdata(parent_loop, &pdata);

    // Parent handles SIGINT to initiate shutdown
    evio_signal parent_sig_w;
    evio_signal_init(&parent_sig_w, parent_signal_cb, SIGINT);
    evio_signal_start(parent_loop, &parent_sig_w);

    // Block signals before forking to prevent races
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, &oldmask);

    // Fork children and watch them with pidfd
    for (int i = 0; i < NUM_CHILDREN; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return EXIT_FAILURE; // Or more robust cleanup
        }

        if (pid == 0) {
            // Child process: clean up parent resources and run its own logic
            free(pdata.children);
            evio_loop_free(parent_loop);
            run_child(listen_fd, &oldmask);
            return 0; // Should not be reached
        }

        // Parent process: store child info and watch its pidfd
        pdata.children[i].pid = pid;
        int pfd = pidfd_open(pid, 0);
        if (pfd < 0) {
            perror("pidfd_open");
            // In a real app, you'd kill the other children and cleanup
            return EXIT_FAILURE;
        }

        evio_poll *pidfd_w = malloc(sizeof(evio_poll));
        if (!pidfd_w) {
            close(pfd);
            // In a real app, you'd kill the other children and cleanup
            return EXIT_FAILURE;
        }

        evio_poll_init(pidfd_w, child_exit_cb, pfd, EVIO_READ);
        evio_poll_start(parent_loop, pidfd_w);

        pdata.children[i].pidfd_w = pidfd_w;
        pdata.children_alive++;
    }

    close(listen_fd); // Parent doesn't need the listening socket anymore

    // Unblock signals now that children are forked and watched
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);

    printf("[Parent] Forked %d children. Waiting for events... (Press Ctrl+C to stop)\n", NUM_CHILDREN);
    evio_run(parent_loop, EVIO_RUN_DEFAULT);

    printf("[Parent] Event loop finished. Exiting.\n");
    free(pdata.children);
    evio_loop_free(parent_loop);

    return EXIT_SUCCESS;
}
