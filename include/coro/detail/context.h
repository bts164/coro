#pragma once

#include <coro/sync/cancellation_token.h>
#include <coro/detail/waker.h>
#include <memory>

namespace coro::detail {

/**
 * @brief Scheduling context passed to every `poll()` / `poll_next()` call.
 *
 * `Context` carries two pieces of per-poll information:
 * - A @ref Waker that a suspended future must store and call when it becomes ready.
 * - An optional `CancellationToken` (reserved for a future cancellation phase).
 *
 * Leaf futures (I/O, timers, channels) call `getWaker()`, store the result, and call
 * `wake()` from their completion callback to move the task back into the ready queue.
 *
 * Concrete executor implementations may subclass `Context` to carry additional
 * scheduler-specific state without changing the `poll()` signature.
 */
class Context {
public:
    explicit Context(std::shared_ptr<Waker> waker,
                     std::shared_ptr<CancellationToken> token = nullptr);
    virtual ~Context();

    /// @brief Returns the waker for the current task. Store this and call `wake()` when ready.
    std::shared_ptr<Waker>             getWaker() const;

    /// @brief Returns the cancellation token, if one was provided. May be null.
    std::shared_ptr<CancellationToken> getCancellationToken() const;

private:
    std::shared_ptr<Waker>             m_waker;
    std::shared_ptr<CancellationToken> m_token;
};

} // namespace coro::detail
