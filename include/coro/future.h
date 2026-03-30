#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>

namespace coro {

// A type F satisfies Future if it has:
//   - F::OutputType  — the result type produced when the future completes
//   - PollResult<F::OutputType> F::poll(Context&)  — advances the future
//
// Mirrors Rust's Future trait. poll() should never be called after it
// returns a Ready or Error result.
template<typename F>
concept Future = requires(F& f, detail::Context& ctx) {
    typename F::OutputType;
    { F(std::move(f)) };
    { f.poll(ctx) } -> std::same_as<PollResult<typename F::OutputType>>;
};

} // namespace coro
