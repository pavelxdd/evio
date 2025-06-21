#include "evio_core.h"
#include "evio_signal.h"

static evio_sig evio_signals[NSIG - 1];

/**
 * @brief The actual POSIX signal handler.
 * @details This function is async-signal-safe. It notes that a signal occurred
 * and writes to the eventfd to wake up the event loop for processing.
 * @param signum The signal number that was caught.
 */
static void evio_signal_cb(int signum)
{
    // GCOVR_EXCL_START
    if (__evio_unlikely(signum <= 0 || signum >= NSIG)) {
        return;
    }
    // GCOVR_EXCL_STOP

    evio_sig *sig = &evio_signals[signum - 1];

    evio_loop *loop = atomic_load_explicit(&sig->loop, memory_order_acquire);
    // GCOVR_EXCL_START
    if (__evio_unlikely(!loop)) {
        return;
    }
    // GCOVR_EXCL_STOP

    atomic_store_explicit(&sig->status.value, 1, memory_order_release);

    if (!atomic_exchange_explicit(&loop->signal_pending.value, 1, memory_order_acq_rel)) {
        evio_eventfd_write(loop);
    }
}

void evio_signal_queue_events(evio_loop *loop, int signum)
{
    evio_sig *sig = &evio_signals[signum - 1];

    const evio_loop *ptr = atomic_load_explicit(&sig->loop, memory_order_acquire);
    if (__evio_unlikely(!ptr || ptr != loop)) {
        return;
    }

    atomic_store_explicit(&sig->status.value, 0, memory_order_release);

    for (size_t i = sig->list.count; i--;) {
        evio_signal *w = (evio_signal *)(sig->list.ptr[i]);
        evio_queue_event(loop, &w->base, EVIO_SIGNAL);
    }
}

void evio_signal_process_pending(evio_loop *loop)
{
    if (atomic_exchange_explicit(&loop->signal_pending.value, 0, memory_order_acq_rel)) {
        for (int i = NSIG - 1; i--;) {
            evio_sig *sig = &evio_signals[i];

            if (atomic_load_explicit(&sig->loop, memory_order_acquire) != loop) {
                continue;
            }

            if (atomic_exchange_explicit(&sig->status.value, 0, memory_order_acq_rel)) {
                for (size_t j = sig->list.count; j--;) {
                    evio_signal *w = (evio_signal *)(sig->list.ptr[j]);
                    evio_queue_event(loop, &w->base, EVIO_SIGNAL);
                }
            }
        }
    }
}

void evio_signal_cleanup_loop(evio_loop *loop)
{
    for (int i = NSIG - 1; i--;) {
        evio_sig *sig = &evio_signals[i];

        if (atomic_load_explicit(&sig->loop, memory_order_acquire) == loop) {
            sig->list.count = 0;
            sig->list.total = 0;

            evio_free(sig->list.ptr);
            sig->list.ptr = NULL;

            sigaction(i + 1, &sig->sa_old, NULL);

            atomic_store_explicit(&sig->loop, NULL, memory_order_release);
        }
    }
}

void evio_signal_start(evio_loop *loop, evio_signal *w)
{
    EVIO_ASSERT(w->signum > 0);
    EVIO_ASSERT(w->signum < NSIG);

    if (__evio_unlikely(w->active)) {
        return;
    }

    evio_sig *sig = &evio_signals[w->signum - 1];

    evio_loop *ptr = atomic_exchange_explicit(&sig->loop, loop, memory_order_acq_rel);
    if (__evio_unlikely(ptr && ptr != loop)) {
        EVIO_ABORT("Invalid loop (%p) signal %d\n", (void *)loop, w->signum);
    }

    w->active = ++sig->list.count;
    evio_ref(loop);

    sig->list.ptr = evio_list_resize(sig->list.ptr, sizeof(*sig->list.ptr),
                                     sig->list.count, &sig->list.total);
    sig->list.ptr[w->active - 1] = &w->base;

    if (sig->list.count == 1) {
        evio_eventfd_init(loop);

        struct sigaction sa = { 0 };
        sa.sa_handler = evio_signal_cb;
        sigfillset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;

        sigaction(w->signum, &sa, &sig->sa_old);
    }
}

void evio_signal_stop(evio_loop *loop, evio_signal *w)
{
    evio_clear_pending(loop, &w->base);

    if (__evio_unlikely(!w->active)) {
        return;
    }

    EVIO_ASSERT(w->signum > 0);
    EVIO_ASSERT(w->signum < NSIG);

    evio_sig *sig = &evio_signals[w->signum - 1];

    sig->list.ptr[w->active - 1] = sig->list.ptr[--sig->list.count];
    sig->list.ptr[w->active - 1]->active = w->active;

    if (sig->list.count == 0) {
        sig->list.total = 0;

        evio_free(sig->list.ptr);
        sig->list.ptr = NULL;

        sigaction(w->signum, &sig->sa_old, NULL);

        atomic_store_explicit(&sig->loop, NULL, memory_order_release);
    }

    evio_unref(loop);
    w->active = 0;
}
