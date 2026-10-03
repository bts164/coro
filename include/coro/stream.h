#pragma once

#include <coro/future.h>
#include <optional>
#include <type_traits>

namespace coro {

namespace detail {

/**
 * Maps a stream's ItemType to its exhaustion-sentinel type:
 *   void  → bool          (true = item completed, false = exhausted)
 *   T     → std::optional<T>
 *
 * std::optional<void> is ill-formed, so void streams use bool instead.
 */
template<typename T>
using StreamItem = std::conditional_t<std::is_void_v<T>, bool, std::optional<T>>;

} // namespace detail

/**
 * @brief C++20 concept modelling an asynchronous sequence of values.
 *
 * Mirrors Rust's `Stream` trait. A type `S` satisfies `Stream` if it exposes:
 * - `S::ItemType` — the element type yielded by the stream.
 * - `PollResult<detail::StreamItem<S::ItemType>> S::poll_next(Context&)` — advances the stream.
 *
 * `poll_next()` return values for non-void streams:
 * - `Ready(optional<T>)` — next item available, or `nullopt` when exhausted.
 * - `Pending`            — no item ready; waker in `ctx` has been registered.
 * - `Error`              — stream faulted; exception embedded in the return value.
 *
 * For `void` streams, `optional<T>` is replaced by `bool` (`true` = item, `false` = exhausted).
 *
 * @tparam S The candidate type to check.
 */
template<typename S>
concept Stream = requires(S& s, detail::Context& ctx) {
    typename S::ItemType;
    { s.poll_next(ctx) } -> std::same_as<PollResult<detail::StreamItem<typename S::ItemType>>>;
};

/**
 * @brief Adapts a @ref Stream into a @ref Future so it can be `co_await`-ed in a loop.
 *
 * Holds a **reference** to the stream — the stream must outlive the `NextFuture`.
 *
 * Typical usage:
 * @code
 * while (auto item = co_await next(stream)) { ... }   // non-void: optional<T>
 * while (co_await next(void_stream)) { ... }          // void: bool
 * @endcode
 *
 * @tparam S A type satisfying @ref Stream.
 */
template<Stream S>
class NextFuture {
public:
    using OutputType = detail::StreamItem<typename S::ItemType>;

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

#ifndef CORO_PICO

/**
 * @brief Pulls exactly one item from @p stream, blocking the calling OS thread until it's
 * available. Equivalent to `blocking_wait(next(stream))`.
 *
 * Same runtime-context requirement as @ref blocking_wait: a future that touches the
 * runtime requires an active `current_runtime()` on the calling thread (ambient on a
 * `spawn_blocking` thread). See doc/design/blocking_wait.md.
 *
 * @return `nullopt`/`false` once the stream is exhausted (matching `next()`'s return type
 * for the stream's `ItemType` — `bool` for `void` streams, `optional<T>` otherwise).
 */
template<Stream S>
detail::StreamItem<typename S::ItemType> blocking_next(S& stream) {
    return blocking_wait(next(stream));
}

#endif // CORO_PICO

} // namespace coro
