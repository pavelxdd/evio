#pragma once

#include <signal.h>

#ifdef EVIO_TESTING

void evio_signal_test_inject_sigaction_fail_once(int err);

int evio_test_sigaction(int signum, const struct sigaction *act,
                        struct sigaction *oldact);
#define EVIO_SIGACTION(signum, act, oldact) \
    evio_test_sigaction((signum), (act), (oldact))

#else // EVIO_TESTING

#define EVIO_SIGACTION(signum, act, oldact) \
    sigaction((signum), (act), (oldact))

#endif // EVIO_TESTING
