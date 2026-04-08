# Select

## Overview

`select` is a Future combinator that runs multiple futures concurrently on the same task
and completes when the first one produces a result, dropping (cancelling) the rest.
Inspired by Tokio's `select!` macro, adapted for C++ without macros.

Inner futures are **concurrent but not parallel** — they run on the same task, interleaving
via suspension points. For parallel first-one-wins, spawn each future and `select` over the
`JoinHandle`s. Branch polling order advances round-robin each poll to prevent starvation.

## Interface

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

## SelectFuture

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

## How poll() works

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

## Design decisions

1. **Result type** — `SelectBranch<N, T>` tagged wrappers in a `std::variant`. Each
   branch result is wrapped in `SelectBranch<N, T>` where N is the branch index. This
   avoids duplicate-type issues when branches have the same `OutputType` (including `void`),
   and lets the caller use `std::holds_alternative<SelectBranch<0, T>>` to identify the
   winning branch. For `void` branches, `SelectBranch<N, void>` carries no value — only
   the index.

2. **Homogeneous select** — Handled automatically by the `SelectBranch<N, T>` tagging.
   `select(f1, f2)` where both return `void` produces
   `std::variant<SelectBranch<0,void>, SelectBranch<1,void>>` — the two alternatives are
   distinct types so the variant is well-formed.

3. **Error handling** — First `Ready` OR `Error` wins. Whichever branch resolves first
   (value or error) is treated as the winner; all other branches are cancelled and drained.
   Errors from non-winning branches are silently dropped.

4. **Fairness** — Round-robin: `SelectFuture` keeps a `m_poll_start` index that advances
   by one each poll. Branches are polled starting from `m_poll_start % N`, wrapping around.
   No RNG needed; this is fair and deterministic.

5. **select over streams** — Out of scope; addressed separately if needed.

## Cancellation and drain

When a branch wins, each losing branch that satisfies `Cancellable` (i.e., has a `cancel()`
method — currently `Coro<T>` and `CoroStream<T>`) is cancelled and then polled until it
returns `PollDropped`. Non-`Cancellable` futures are dropped immediately. `SelectFuture`
holds the winning result internally and does not return it until all cancelled branches have
returned `PollDropped`.
