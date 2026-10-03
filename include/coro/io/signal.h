#pragma once

// Desktop only: Unix signals as futures and streams, on the IoDriver. See
// doc/design/signal_handling.md.

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <csignal>  // SIGINT etc.: callers name the signals they pass
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>

namespace coro {

/**
 * @brief One coalesced batch of deliveries for a single signal number.
 *
 * Yielded by @ref SignalStream. `count` is a guaranteed **lower bound** on how many
 * times `signum` actually fired since the previous item for that signum was yielded:
 * never an overcount, but possibly an undercount for standard (non-realtime) signals
 * the kernel coalesced before the handler ran. See doc/design/signal_handling.md
 * ("Delivery counting").
 */
struct SignalEvent {
    int      signum;
    uint64_t count;
};

namespace detail {
struct SignalState;   // defined in signal.cpp
} // namespace detail

/**
 * @brief Stream of coalesced signal-delivery events. Obtain via @ref signal_stream.
 *
 * Satisfies the @ref Stream concept (`ItemType = SignalEvent`). Never exhausts.
 * Watching starts when the stream is created, not at its first poll, and stops
 * synchronously when it is destroyed.
 *
 * Only one task may poll a given stream at a time.
 */
class [[nodiscard]] SignalStream {
public:
    using ItemType = SignalEvent;

    explicit SignalStream(std::unique_ptr<detail::SignalState> state) noexcept;

    SignalStream(SignalStream&&) noexcept;
    SignalStream& operator=(SignalStream&&) noexcept;
    SignalStream(const SignalStream&)            = delete;
    SignalStream& operator=(const SignalStream&) = delete;

    /// Stops watching. The last watcher of a signal restores its previous action.
    ~SignalStream();

    PollResult<std::optional<SignalEvent>> poll_next(detail::Context& ctx);

private:
    std::unique_ptr<detail::SignalState> m_state;
};

/**
 * @brief Future resolving on the first delivery of `signum` after it was created.
 * Obtain via @ref signal.
 *
 * Satisfies the @ref Future concept (`OutputType = void`). A leaf future: dropping it
 * at any time just stops watching.
 */
class [[nodiscard]] SignalFuture {
public:
    using OutputType = void;

    explicit SignalFuture(std::unique_ptr<detail::SignalState> state) noexcept;

    SignalFuture(SignalFuture&&) noexcept;
    SignalFuture& operator=(SignalFuture&&) noexcept;
    SignalFuture(const SignalFuture&)            = delete;
    SignalFuture& operator=(const SignalFuture&) = delete;

    /// Stops watching. The last watcher of a signal restores its previous action.
    ~SignalFuture();

    PollResult<void> poll(detail::Context& ctx);

private:
    std::unique_ptr<detail::SignalState> m_state;
};

/**
 * @brief Resolves once, on the next delivery of `signum`.
 *
 * Watching starts immediately, so a signal delivered between this call and the first
 * `co_await` is not missed. While any watcher of `signum` exists, its previous action
 * (e.g. SIGINT's default, terminate) is replaced by coro's handler; the previous action
 * is restored when the last watcher is dropped. A handler the application installed
 * itself is replaced too, not chained.
 *
 * @throws std::system_error EINVAL if `signum` can't be caught (SIGKILL, SIGSTOP, or out
 *         of range).
 * @throws std::logic_error if the current Runtime's executor doesn't turn the IoDriver.
 */
[[nodiscard]] SignalFuture signal(int signum);

/// @brief Yields a coalesced SignalEvent for every distinct watched signal that has
/// fired at least once since the last item. Same watching rules and exceptions as
/// @ref signal.
[[nodiscard]] SignalStream signal_stream(std::initializer_list<int> signums);

} // namespace coro
