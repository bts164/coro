#pragma once

#include <coro/future.h>
#include <optional>

namespace coro {

// A type S satisfies Stream if it has:
//   - S::ItemType  — the element type yielded by the stream
//   - PollResult<std::optional<S::ItemType>> S::poll_next(Context&)
//       Ready(some(T))  — next item is available
//       Ready(nullopt)  — stream is exhausted
//       Pending         — no item ready; waker registered for later wake-up
//       Error           — stream faulted; exception captured in PollResult
//
// Mirrors Rust's Stream trait.
template<typename S>
concept Stream = requires(S& s, detail::Context& ctx) {
    typename S::ItemType;
    { s.poll_next(ctx) } -> std::same_as<PollResult<std::optional<typename S::ItemType>>>;
};

// Adapts a Stream into a Future<optional<T>> so it can be co_awaited in a loop.
// Holds a reference to the stream — the stream must outlive the NextFuture.
// Usage: while (auto item = co_await next(stream)) { ... }
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

template<Stream S>
NextFuture<S> next(S& stream) {
    return NextFuture<S>(stream);
}

} // namespace coro
