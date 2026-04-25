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

} // namespace coro
