#pragma once

#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <optional>
#include <type_traits>
#include <utility>

namespace coro {

/**
 * @brief Future wrapper that conditionally holds an inner future, constructed lazily.
 *
 * Returned by @ref when. When disengaged, `WhenFuture` never constructs `F` — not even
 * a default instance — so it doubles as a static type placeholder for branches whose
 * construction is only sometimes valid, or even possible. A disengaged `WhenFuture`
 * polls exactly like `NeverFuture<T>` (`coro/future.h`): always `PollPending`.
 *
 * Prefer the @ref when factory function over constructing this directly.
 *
 * @tparam F A type satisfying @ref Future.
 */
template<Future F>
class [[nodiscard]] WhenFuture {
public:
    using OutputType = typename F::OutputType;

    /// Disengaged — `F` is never constructed.
    WhenFuture() noexcept = default;
    explicit WhenFuture(F future) : m_future(std::move(future)) {}

    WhenFuture(WhenFuture&&) noexcept            = default;
    WhenFuture& operator=(WhenFuture&&) noexcept = default;
    WhenFuture(const WhenFuture&)                = delete;
    WhenFuture& operator=(const WhenFuture&)     = delete;

    PollResult<OutputType> poll(detail::Context& ctx) {
        if (!m_future) {
            if (m_cancelled) {
                return PollDropped;
             } else {
                return PollPending;
             }
        }
        return m_future->poll(ctx);
    }

    // Forwards cancellation to the inner future when engaged. When disengaged there is
    // nothing to cancel, but poll() must still eventually report PollDropped so that
    // select()'s drain loop (which polls every Cancellable losing branch until it sees
    // PollDropped) terminates instead of spinning on a WhenFuture that can never
    // otherwise leave PollPending.
    void cancel() requires Cancellable<F> {
        if (m_future) m_future->cancel();
        else m_cancelled = true;
    }

private:
    std::optional<F> m_future;
    bool             m_cancelled = false;  // only meaningful while disengaged
};

/**
 * @brief Conditionally constructs and wraps a future, evaluated lazily.
 *
 * `make_future` is invoked at most once, and only when `cond` is true — when `cond`
 * is false, the wrapped future type `F` is never constructed. This makes `when()`
 * usable even when constructing the branch is impossible (not just expensive) in the
 * disabled case: the disengaged `WhenFuture` is just a static type placeholder.
 *
 * Typical usage — selecting on a branch that's only sometimes meaningful:
 * @code
 * auto result = co_await select(listener.accept(),
 *                                when(!clients.empty(), [&]{ return next(clients); }),
 *                                ref(sigint));
 * @endcode
 *
 * If `make_future` would be expensive to re-invoke on every loop iteration, construct
 * the inner future once outside the loop and pass it through `coro::ref()` instead.
 *
 * @param cond Whether to construct and poll the inner future.
 * @param make_future Invoked at most once, only when `cond` is true. Must return a
 *        type satisfying @ref Future.
 */
template<typename Fn>
    requires (!Future<Fn> && Future<std::invoke_result_t<Fn>>)
[[nodiscard]] auto when(bool cond, Fn&& make_future) {
    using F = std::invoke_result_t<Fn>;
    if (cond) return WhenFuture<F>(std::forward<Fn>(make_future)());
    return WhenFuture<F>();
}

} // namespace coro
