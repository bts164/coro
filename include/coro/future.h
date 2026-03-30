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

} // namespace coro
