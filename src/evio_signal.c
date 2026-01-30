#include "evio_core.h"
#include "evio_signal.h"
#include "evio_signal_sys.h"

static evio_sig evio_signals[NSIG - 1];

static inline void evio_signal_active_set(evio_loop *loop, int signum)
{
    EVIO_ASSERT(signum > 0);
    EVIO_ASSERT(signum < NSIG);

    unsigned idx = (unsigned)(signum - 1);
    loop->sig_active[idx >> 6] |= 1ull << (idx & 63);
}

static inline void evio_signal_active_clear(evio_loop *loop, int signum)
{
    EVIO_ASSERT(signum > 0);
    EVIO_ASSERT(signum < NSIG);

    unsigned idx = (unsigned)(signum - 1);
    loop->sig_active[idx >> 6] &= ~(1ull << (idx & 63));
}

/**
 * @brief The actual POSIX signal handler.
 * @details Async-signal-safe: set flag + wake loop via eventfd.
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
        evio_signal *w = container_of(sig->list.ptr[i], evio_signal, base);
        evio_queue_event(loop, &w->base, EVIO_SIGNAL);
    }
}

void evio_signal_process_pending(evio_loop *loop)
{
    if (atomic_exchange_explicit(&loop->signal_pending.value, 0, memory_order_acq_rel)) {
        for (size_t wi = 0; wi < EVIO_SIGSET_WORDS; ++wi) {
            uint64_t bits = loop->sig_active[wi];
            while (bits) {
                unsigned b = (unsigned)__builtin_ctzll(bits);
                unsigned idx = (unsigned)(wi * 64u + b);
                if (__evio_unlikely(idx >= (unsigned)(NSIG - 1))) {
                    break; // GCOVR_EXCL_LINE
                }

                bits &= bits - 1;

                evio_sig *sig = &evio_signals[idx];

                if (__evio_unlikely(atomic_load_explicit(&sig->loop, memory_order_acquire) != loop)) {
                    continue;
                }

                if (atomic_exchange_explicit(&sig->status.value, 0, memory_order_acq_rel)) {
                    for (size_t j = sig->list.count; j--;) {
                        evio_signal *w = container_of(sig->list.ptr[j], evio_signal, base);
                        EVIO_ASSERT(w->signum == (int)idx + 1);
                        evio_queue_event(loop, &w->base, EVIO_SIGNAL);
                    }
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
            int rc = EVIO_SIGACTION(i + 1, &sig->sa_old, NULL);
            EVIO_ASSERT(rc == 0);
            (void)rc;

            sig->list.count = 0;
            sig->list.total = 0;

            evio_free(sig->list.ptr);
            sig->list.ptr = NULL;

            // Reset the pending status to prevent stale signal delivery.
            atomic_store_explicit(&sig->status.value, 0, memory_order_release);

            evio_signal_active_clear(loop, i + 1);
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
    if (sig->list.count == 0) {
        evio_eventfd_init(loop);

        struct sigaction sa = { 0 };
        sa.sa_handler = evio_signal_cb;
        sigfillset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;

        int rc = EVIO_SIGACTION(w->signum, &sa, &sig->sa_old);
        EVIO_ASSERT(rc == 0);
        (void)rc;

        evio_signal_active_set(loop, w->signum);
    }

    w->active = ++sig->list.count;
    evio_ref(loop);

    sig->list.ptr = evio_list_resize(sig->list.ptr, sizeof(*sig->list.ptr),
                                     sig->list.count, &sig->list.total);
    sig->list.ptr[w->active - 1] = &w->base;
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

    if (sig->list.count == 1) {
        int rc = EVIO_SIGACTION(w->signum, &sig->sa_old, NULL);
        EVIO_ASSERT(rc == 0);
        (void)rc;
    }

    sig->list.ptr[w->active - 1] = sig->list.ptr[--sig->list.count];
    sig->list.ptr[w->active - 1]->active = w->active;

    if (sig->list.count == 0) {
        sig->list.total = 0;

        evio_free(sig->list.ptr);
        sig->list.ptr = NULL;

        // Reset the pending status to prevent stale signal delivery.
        atomic_store_explicit(&sig->status.value, 0, memory_order_release);

        evio_signal_active_clear(loop, w->signum);
        atomic_store_explicit(&sig->loop, NULL, memory_order_release);
    }

    evio_unref(loop);
    w->active = 0;
}
