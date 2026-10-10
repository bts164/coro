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

namespace detail {

/**
 * @brief The future `blocking_next()` waits on: `next(stream)`, plus the means to shut
 * the stream down when the blocking task is cancelled.
 *
 * `NextFuture` is deliberately not @ref Cancellable. It borrows the stream, and a
 * `select()` that drops a losing `next(stream)` branch must leave the stream running for
 * the next round. `blocking_next()` is different: when it is cancelled the caller's stack
 * is about to unwind and take the stream with it, so the stream has to be cancelled and
 * drained first, as a cancelled task drains the stream it runs. This adapter is
 * `Cancellable` exactly when the stream has a `cancel()`; `blocking_wait()` then runs the
 * drain. A stream with no `cancel()` (a channel receiver) is a leaf and is left alone.
 */
template<Stream S>
class BlockingNextFuture {
public:
    using OutputType = StreamItem<typename S::ItemType>;

    explicit BlockingNextFuture(S& stream) : m_stream(stream) {}

    PollResult<OutputType> poll(Context& ctx) {
        for (;;) {
            auto r = m_stream.poll_next(ctx);
            if (!m_draining || r.isPending() || r.isDropped() || r.isError()) return r;
            // Draining. A stream that buffers (StreamHandle) hands out the items it
            // already holds before it reports that its producer has stopped; discard
            // them and keep going until it is exhausted.
            OutputType item = std::move(r).value();
            if (!item) return PollResult<OutputType>(std::move(item));
        }
    }

    void cancel() requires requires(S& s) { s.cancel(); } {
        m_stream.cancel();
        m_draining = true;
    }

private:
    S&   m_stream;
    bool m_draining = false;
};

} // namespace detail

/**
 * @brief Pulls exactly one item from @p stream, blocking the calling OS thread until it's
 * available. The blocking counterpart of `co_await next(stream)`.
 *
 * Same runtime-context requirement as @ref blocking_wait: a future that touches the
 * runtime requires an active `current_runtime()` on the calling thread (ambient on a
 * `spawn_blocking` thread). See doc/design/blocking_wait.md.
 *
 * **Cancellation.** A cancellation point, like @ref blocking_wait. If the blocking task
 * is cancelled and @p stream has a `cancel()` (a `CoroStream`, a `StreamHandle`), the
 * stream is cancelled and polled until it has drained before @ref BlockingCancelled is
 * thrown. The stream is finished after that; all that is left to do with it is destroy
 * it.
 *
 * @return `nullopt`/`false` once the stream is exhausted (matching `next()`'s return type
 * for the stream's `ItemType` — `bool` for `void` streams, `optional<T>` otherwise).
 */
template<Stream S>
detail::StreamItem<typename S::ItemType> blocking_next(S& stream) {
    return blocking_wait(detail::BlockingNextFuture<S>(stream));
}

#endif // CORO_PICO

} // namespace coro
