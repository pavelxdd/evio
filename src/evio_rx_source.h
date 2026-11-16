#pragma once

/**
 * @file evio_rx_source.h
 * @brief Cold Rx sources: user producers, arrays, timers and fd readiness.
 */

#include "evio_rx.h"

/** @brief The producer handle passed to user source callbacks. */
typedef struct evio_rx_emitter evio_rx_emitter;

/**
 * @brief A user producer protocol.
 * @details `prepare` creates per-run state without notifying and owns its rollback;
 * `*state` is NULL on entry. `start` may notify synchronously. `stop` runs once
 * per `start`, possibly inside it, and disconnects external callbacks without
 * freeing state. `destroy` runs once after a successful
 * prepare, after all protected callbacks have returned. `prepare` and `start` are
 * required.
 */
typedef struct {
    int (*prepare)(void *ctx, evio_loop *loop, evio_rx_emitter *emitter, void **state);
    void (*start)(evio_rx_emitter *emitter, void *state);
    void (*stop)(void *state);
    void (*destroy)(void *state);
} evio_rx_source;

/** @brief A protected producer callback, see `evio_rx_emitter_invoke()`. */
typedef void (*evio_rx_source_cb)(evio_rx_emitter *emitter, void *state, void *data);

/** @brief A readiness event of `evio_rx_from_poll()`. */
typedef struct {
    int fd;
    evio_mask events;
} evio_rx_poll_event;

/** @brief `uint64_t` values of `evio_rx_timer()` and `evio_rx_interval()`. */
__evio_public
extern const evio_rx_type evio_rx_uint64_type;

/** @brief `evio_rx_poll_event` values of `evio_rx_from_poll()`. */
__evio_public
extern const evio_rx_type evio_rx_poll_event_type;

/**
 * @brief Creates a cold description with one user producer per run.
 * @param type The value type.
 * @param source The producer protocol, copied.
 * @param capture Passed to `prepare` as `ctx`.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_observable_new(const evio_rx_type *type, evio_rx_source source,
                                           evio_rx_capture capture);

/**
 * @brief Runs a native producer callback under Rx protection.
 * @details Call immediately on entry to a native callback, before work that can
 * reenter Rx. `emitter` and its state stay alive within `cb`; do not access the
 * state after this returns. Does nothing once the producer is closed.
 * @param emitter The producer.
 * @param cb The callback.
 * @param data Borrowed argument of `cb`.
 */
__evio_public __evio_nonnull(1, 2)
void evio_rx_emitter_invoke(evio_rx_emitter *emitter, evio_rx_source_cb cb, void *data);

/**
 * @brief Delivers a value.
 * @param emitter The producer.
 * @param value The borrowed value; NULL fails the run with EINVAL.
 * @return `false` if production must stop, including cancel during delivery.
 */
__evio_public __evio_nonnull(1) __evio_nodiscard
bool evio_rx_emitter_next(evio_rx_emitter *emitter, const void *value);

/**
 * @brief Fails the producer; a non-positive code becomes EINVAL.
 * @param emitter The producer.
 * @param error The errno-compatible code.
 */
__evio_public __evio_nonnull(1)
void evio_rx_emitter_error(evio_rx_emitter *emitter, int error);

/**
 * @brief Completes the producer.
 * @param emitter The producer.
 */
__evio_public __evio_nonnull(1)
void evio_rx_emitter_complete(evio_rx_emitter *emitter);

/**
 * @brief Creates a synchronous source of a snapshot of `items`.
 * @details Delivers values in batches of up to 64; input queued during a batch
 * is delivered after it.
 * @param type The value type; `items` has a stride of `type->size`.
 * @param items The values; NULL only when `count` is 0.
 * @param count The number of values.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_from_array(const evio_rx_type *type, const void *items, size_t count);

/**
 * @brief Creates a source emitting 0 once, then complete.
 * @details Never notifies inside `evio_rx_subscription_start()`, even for 0.
 * @param after The delay in nanoseconds.
 * @return The description.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_timer(evio_time after);

/**
 * @brief Creates a source emitting 0, 1, 2, ...
 * @details Each tick is scheduled from the cached time of processing the previous
 * one; missed periods are not replayed.
 * @param after The delay of the first value in nanoseconds.
 * @param period The positive period in nanoseconds.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_interval(evio_time after, evio_time period);

/**
 * @brief Creates a source of fd readiness, one poll watcher per run.
 * @details The application owns reads, EOF handling and the fd. `EVIO_ERROR`
 * fails the run with EIO.
 * @param fd The file descriptor.
 * @param events A nonempty mask of `EVIO_READ` and `EVIO_WRITE`.
 * @return The description, or NULL with errno set.
 */
__evio_public __evio_nodiscard
evio_rx_observable *evio_rx_from_poll(int fd, evio_mask events);
