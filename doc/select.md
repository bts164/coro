# Select

## Overview

`select` is a Future combinator that runs multiple futures concurrently on the same task
and completes when the first one produces a result, dropping (cancelling) the rest.
Inspired by Tokio's `select!` macro, adapted for C++ without macros.

## Requirements

- `select(f1, f2, ...)` accepts any number of futures, potentially of different output types
- Returns a type satisfying `Future`
- Completes when the first inner future returns `Ready`, drops the remaining futures
- If all inner futures return `Pending`, registers the outer task's waker via the shared
  `Context` and returns `Pending`
- Inner futures are **concurrent but not parallel** — they run on the same task and thread,
  interleaving via suspension points
- For parallel first-one-wins, spawn each future and `select` over the `JoinHandle`s
- Branch polling order is randomized each poll to prevent starvation

## Proposed Design

### Interface

```cpp
// Returns SelectFuture<F1, F2, F3> which satisfies Future<std::variant<T1, T2, T3>>
auto result = co_await select(
    timeout(5s),
    read_packet(sock),
    recv_signal()
);
// result is std::variant<TimeoutResult, Packet, Signal>
std::visit([](auto& val) { ... }, result);
```

### SelectFuture

```cpp
template<Future... Fs>
class SelectFuture {
public:
    using OutputType = std::variant<typename Fs::OutputType...>;

    explicit SelectFuture(Fs&&... futures);

    PollResult<OutputType> poll(Context& ctx);

private:
    std::tuple<Fs...> m_futures;
    // Polling order is shuffled each call to poll() for fairness
};

// Factory function
template<Future... Fs>
SelectFuture<Fs...> select(Fs&&... futures);
```

### How poll() works

1. Shuffle the poll order for this tick (fairness — avoids starvation of later futures)
2. Poll each inner future in shuffled order, passing the same `Context` through
   - All inner futures register against the same waker, so any one of them waking the
     outer task causes a full re-poll of `SelectFuture`
3. First inner future that returns `Ready(value)` wins:
   - Wrap the value in the corresponding `std::variant` alternative
   - Drop all remaining inner futures (triggering cancellation)
   - Return `Ready(variant)`
4. First inner future that returns `Error` propagates immediately (same drop behaviour)
5. If all return `Pending`, return `Pending`

## Open Questions

1. **Result type** — `std::variant<T1, T2, T3>` requires `std::visit` or index checking
   to determine which branch won. Is there a more ergonomic alternative that still works
   without macros? One option: a tagged struct `SelectResult<N, variant>` that exposes
   the winning index as a compile-time constant. Another: separate `select_index()` that
   returns the index alongside the variant.

2. **Homogeneous select** — If all futures have the same `OutputType`, a `std::variant`
   with identical types is awkward. Should `select` special-case this to return
   `OutputType` directly (without variant) when all types match?

3. **Error handling** — If the winning future returns `Error`, that propagates. But what
   if a non-winning future returns `Error` before the winner does? Options:
   - First `Ready` or `Error` wins, same as `Ready`
   - Only the first `Ready` wins; `Error` from non-winners is silently dropped
   - Any `Error` from any branch propagates immediately

4. **Fairness implementation** — Randomizing poll order requires a RNG. Should this be
   seeded per-task, per-`SelectFuture` instance, or use a thread-local? A biased or
   round-robin scheme could be simpler and still fair enough in practice.

5. **select over streams** — Should there be a `select_stream(s1, s2, ...)` variant that
   merges multiple streams into one, yielding items from whichever stream is ready first?
   This is Tokio's `tokio_stream::StreamExt::merge()`. Separate combinator or unified API?
