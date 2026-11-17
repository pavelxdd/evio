#pragma once

#include <poll.h>

#include "test.h"

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
