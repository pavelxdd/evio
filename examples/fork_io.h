#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "evio.h"

#define BUFFER_SIZE 1024

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

typedef struct {
    evio_poll io;
    evio_timer retry;
} listener_info;

static void accept_retry_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    listener_info *listener = base->data;
    evio_poll_start(loop, &listener->io);
}

// Callback to handle new connections on the listening socket
static void accept_cb(evio_loop *loop, evio_base *base, evio_mask emask)
{
    listener_info *listener = base->data;
    evio_poll *w = &listener->io;
    struct sockaddr_in client_addr = { 0 };
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept4(w->fd, (struct sockaddr *)&client_addr, &client_len, SOCK_NONBLOCK);
    if (client_fd < 0) {
        int err = errno;
        if (err == EAGAIN || err == EINTR) {
            return;
        }
        if (err == EMFILE || err == ENFILE) {
            evio_poll_stop(loop, w);
            evio_update_time(loop);
            evio_timer_start(loop, &listener->retry, EVIO_TIME_FROM_MSEC(100));
        }
        errno = err;
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
