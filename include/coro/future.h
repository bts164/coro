#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>

namespace coro {

/**
 * @brief C++20 concept modelling an asynchronous value.
 *
 * Mirrors Rust's `Future` trait. A type `F` satisfies `Future` if it exposes:
 * - `F::OutputType` — the type produced when the future completes.
 * - `PollResult<F::OutputType> F::poll(Context&)` — advances the future toward completion.
 *
 * `poll()` returns one of four states:
 * - `PollPending`  — not yet ready; the waker in `ctx` has been registered for a future wake-up.
 * - `PollReady`    — completed successfully; the result is embedded in the return value.
 * - `PollError`    — faulted; an exception is embedded in the return value.
 * - `PollDropped`  — cancelled and fully drained; propagates up the call chain.
 *
 * `poll()` **must not** be called again after it returns `PollReady`, `PollError`, or `PollDropped`.
 *
 * @tparam F The candidate type to check.
 */
template<typename F>
concept Future = requires(F& f, detail::Context& ctx) {
    typename F::OutputType;
    { F(std::move(f)) };
    { f.poll(ctx) } -> std::same_as<PollResult<typename F::OutputType>>;
};

/**
 * @brief Concept satisfied by futures that own an internal execution tree and must be
 * drained before they can be safely destroyed.
 *
 * A type satisfying `Cancellable` declares: "I cannot simply be dropped mid-execution.
 * Call `cancel()` on me, then poll me until I return `PollDropped`."  The canonical
 * implementations are @ref Coro and @ref CoroStream, which may be awaiting nested
 * children that hold references to the caller's frame locals.
 *
 * A future that does **not** satisfy `Cancellable` is a leaf future: it does not await
 * children, holds no references to the caller's locals, and can be destroyed at any time
 * (its destructor handles immediate cleanup such as removing a stored waker). Examples:
 * channel send/receive futures, `EventFuture`, timer futures.
 *
 * The cancellation protocol in `Coro<T>::poll()` uses this distinction: Cancellable
 * awaited futures are drained before `handle.destroy()` is called; non-Cancellable ones
 * are dropped as part of the LIFO teardown in `handle.destroy()`.
 *
 * @tparam F A type satisfying @ref Future.
 */
template<typename F>
concept Cancellable = Future<F> && requires(F& f) { f.cancel(); };


/**
 * @brief Non-owning wrapper that makes a borrowed future usable where a future is expected.
 *
 * Returned by `coro::ref(f)`. Holds a raw pointer to `f` and delegates `poll()` to the
 * underlying future without taking ownership of it.
 *
 * The primary use-case is passing a future to `select()` (or `join()`, `timeout()`, etc.)
 * without consuming it — if the branch loses, the underlying future keeps running and can
 * be used again in the next round.
 *
 * **Lifetime:** `FutureRef<F>` holds a raw pointer to `f`. `f` must outlive the
 * `FutureRef`. The intended usage pattern — `co_await select(coro::ref(f), other)` — is
 * naturally safe because `f` is a named local in the same scope and the `FutureRef` is a
 * temporary destroyed when the `co_await` returns.
 *
 * **Result consumption:** if the `FutureRef` branch wins and delivers a result, the result
 * is moved out of `f`. Do not await `f` again after that — it is logically spent.
 *
 * **Cancellation:** `FutureRef` is never `Cancellable`, regardless of whether `F` is.
 * When a `select()` branch backed by a `FutureRef` loses, `select()` simply drops the
 * wrapper — the underlying future keeps running untouched. Any waker the losing poll
 * registered remains live and may cause a spurious wake-up on the next select round;
 * this is harmless because `poll()` is required to handle spurious calls gracefully.
 *
 * `FutureRef` is non-copyable and movable.
 *
 * @tparam F A type satisfying @ref Future.
 */
template<Future F>
class FutureRef {
public:
    using OutputType = typename F::OutputType;

    explicit FutureRef(F& f) noexcept : m_future(&f) {}

    FutureRef(const FutureRef&)            = delete;
    FutureRef& operator=(const FutureRef&) = delete;
    FutureRef(FutureRef&&) noexcept            = default;
    FutureRef& operator=(FutureRef&&) noexcept = default;

    PollResult<OutputType> poll(detail::Context& ctx) {
        return m_future->poll(ctx);
    }

    // cancel() is intentionally absent. FutureRef is a non-owning view; cancelling it
    // would cancel the underlying future, which is the opposite of what ref() is for.
    // select() will simply drop the FutureRef when a branch loses, leaving the underlying
    // future running. Any waker the losing poll registered remains live and may fire
    // spuriously — this is accepted as a minor inefficiency; spurious polls are part of
    // the poll() contract.

private:
    F* m_future;
};

/**
 * @brief Wraps `f` in a `FutureRef<F>` — a non-owning future that delegates to `f`.
 *
 * Only accepts lvalues. This prevents accidentally wrapping a temporary (which would
 * immediately dangle). The typical usage:
 *
 * @code
 * JoinHandle<int> task = spawn(work());
 * // task keeps running if the other branch wins:
 * auto sel = co_await select(coro::ref(task), signal);
 * @endcode
 *
 * See @ref FutureRef for the full contract.
 */
template<Future F>
[[nodiscard]] FutureRef<F> ref(F& f) noexcept {
    return FutureRef<F>(f);
}

} // namespace coro
