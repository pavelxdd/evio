#pragma once

#include <sys/types.h>
#include <unistd.h>

#ifdef EVIO_TESTING

void evio_eventfd_test_inject_read_fail_once(int err);
void evio_eventfd_test_inject_write_fail_once(int err);

ssize_t evio_test_eventfd_read(int fd, void *buf, size_t count);
ssize_t evio_test_eventfd_write(int fd, const void *buf, size_t count);

#define EVIO_EVENTFD_READ(fd, buf, count) \
    evio_test_eventfd_read((fd), (buf), (count))
#define EVIO_EVENTFD_WRITE(fd, buf, count) \
    evio_test_eventfd_write((fd), (buf), (count))

#else

#define EVIO_EVENTFD_READ(fd, buf, count) \
    read((fd), (buf), (count))
#define EVIO_EVENTFD_WRITE(fd, buf, count) \
    write((fd), (buf), (count))

#endif
