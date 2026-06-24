#pragma once

#include <coro/detail/context.h>
#include <coro/detail/mutex.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/rc.h>
#include <coro/detail/waker.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/stream.h>
#include <uv.h>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

namespace coro {

/**
 * @brief One coalesced batch of deliveries for a single signal number.
 *
 * Yielded by @ref SignalStream. `count` is a guaranteed **lower bound** on how many
 * times `signum` actually fired since the previous item for that signum was yielded
 * — never an overcount, but possibly an undercount for standard (non-realtime)
 * signals coalesced by the kernel before libuv ever saw them. See
 * doc/design/signal_handling.md ("Delivery counting") for the full guarantee.
 */
struct SignalEvent {
    int      signum;
    uint64_t count;
};

namespace detail {

// Shared between the uv-thread signal callback (signal_cb) and the consumer-side
// poll()/poll_next(). Mutex-protected exactly like Event — no libuv calls happen
// outside setup/teardown, which run via with_context on the uv thread.
struct SignalState {
    coro::detail::Mutex             mutex;
    std::deque<SignalEvent>         pending;      // GUARDED BY mutex; at most one entry per signum
    coro::detail::Rc<coro::detail::Waker> waker;  // GUARDED BY mutex
    bool                            closed = false;       // GUARDED BY mutex
    int                             setup_error = 0;       // GUARDED BY mutex; 0 = ok, else a uv error code
    std::deque<uv_signal_t>         handles;      // uv thread only; deque keeps element addresses
                                                   // stable across growth (see signal_handling.md)
};

// Fired by libuv on the uv thread when a watched signal is delivered. Shared across
// every uv_signal_t handle in a given SignalState.
void signal_cb(uv_signal_t* handle, int signum);

} // namespace detail

/**
 * @brief Stream of coalesced signal-delivery events. Obtain via @ref signal_stream.
 *
 * Satisfies the @ref Stream concept (`ItemType = SignalEvent`). Never naturally
 * exhausts — `poll_next()` only returns `Ready(nullopt)` if explicitly closed, which
 * happens automatically when the `SignalStream` is destroyed.
 *
 * See doc/design/signal_handling.md for the full design.
 */
class [[nodiscard]] SignalStream {
public:
    using ItemType = SignalEvent;

    explicit SignalStream(std::shared_ptr<detail::SignalState> state,
                          SingleThreadedUvExecutor* uv_exec) noexcept;

    SignalStream(SignalStream&&) noexcept;
    SignalStream& operator=(SignalStream&&) noexcept;
    SignalStream(const SignalStream&)            = delete;
    SignalStream& operator=(const SignalStream&) = delete;

    /// Stops and closes every watched uv_signal_t handle asynchronously on the uv executor.
    ~SignalStream();

    PollResult<std::optional<SignalEvent>> poll_next(detail::Context& ctx);

private:
    std::shared_ptr<detail::SignalState> m_state;
    SingleThreadedUvExecutor*            m_uv_exec = nullptr;
};

/**
 * @brief One-shot future resolving on the next delivery of `signum`. Obtain via @ref signal.
 *
 * Satisfies the @ref Future concept (`OutputType = void`). Carries no count — only
 * "has it fired yet" matters for the one-shot case.
 */
class [[nodiscard]] SignalFuture {
public:
    using OutputType = void;

    explicit SignalFuture(std::shared_ptr<detail::SignalState> state,
                         SingleThreadedUvExecutor* uv_exec) noexcept;

    SignalFuture(SignalFuture&&) noexcept;
    SignalFuture& operator=(SignalFuture&&) noexcept;
    SignalFuture(const SignalFuture&)            = delete;
    SignalFuture& operator=(const SignalFuture&) = delete;

    /// Stops and closes the watched uv_signal_t handle asynchronously on the uv executor.
    ~SignalFuture();

    PollResult<void> poll(detail::Context& ctx);

private:
    std::shared_ptr<detail::SignalState> m_state;
    SingleThreadedUvExecutor*            m_uv_exec = nullptr;
};

/// @brief Resolves once, on the next delivery of `signum`.
[[nodiscard]] SignalFuture signal(int signum);

/// @brief Yields a coalesced SignalEvent for every distinct watched signal that has
/// fired at least once since the last poll.
[[nodiscard]] SignalStream signal_stream(std::initializer_list<int> signums);

} // namespace coro
