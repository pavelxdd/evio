#include <sys/epoll.h>

#include "evio_core.h"
#include "evio_uring.h"

evio_uring *evio_uring_new(void)
{
    return NULL;
}

void evio_uring_free(evio_uring *iou)
{
    EVIO_ASSERT(iou);
}

void evio_uring_ctl(evio_loop *loop, int op, int fd, const struct epoll_event *ev)
{
    EVIO_ABORT("Invalid io_uring usage\n");
}

void evio_uring_flush(evio_loop *loop)
{
    EVIO_ABORT("Invalid io_uring usage\n");
}
