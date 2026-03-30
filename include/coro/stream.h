#pragma once

#include <coro/future.h>
#include <optional>

namespace coro {

/**
 * @brief C++20 concept modelling an asynchronous sequence of values.
 *
 * Mirrors Rust's `Stream` trait. A type `S` satisfies `Stream` if it exposes:
 * - `S::ItemType` — the element type yielded by the stream.
 * - `PollResult<std::optional<S::ItemType>> S::poll_next(Context&)` — advances the stream.
 *
 * `poll_next()` return values:
 * - `Ready(some(T))` — next item is available.
 * - `Ready(nullopt)`  — stream is exhausted; no more items will be produced.
 * - `Pending`         — no item is ready yet; the waker in `ctx` has been registered.
 * - `Error`           — stream faulted; an exception is embedded in the return value.
 *
 * @tparam S The candidate type to check.
 */
template<typename S>
concept Stream = requires(S& s, detail::Context& ctx) {
    typename S::ItemType;
    { s.poll_next(ctx) } -> std::same_as<PollResult<std::optional<typename S::ItemType>>>;
};

/**
 * @brief Adapts a @ref Stream into a @ref Future so it can be `co_await`-ed in a loop.
 *
 * Holds a **reference** to the stream — the stream must outlive the `NextFuture`.
 *
 * Typical usage:
 * @code
 * while (auto item = co_await next(stream)) { ... }
 * @endcode
 *
 * @tparam S A type satisfying @ref Stream.
 */
template<Stream S>
class NextFuture {
public:
    using OutputType = std::optional<typename S::ItemType>;

    explicit NextFuture(S& stream) : m_stream(stream) {}

    PollResult<OutputType> poll(detail::Context& ctx) {
        return m_stream.poll_next(ctx);
    }

private:
    S& m_stream;
};

/**
 * @brief Creates a @ref NextFuture that yields the next item from @p stream.
 *
 * @param stream The stream to advance. Must outlive the returned future.
 * @return A `Future<optional<T>>` that resolves to the next item, or `nullopt` on exhaustion.
 */
template<Stream S>
NextFuture<S> next(S& stream) {
    return NextFuture<S>(stream);
}

} // namespace coro
