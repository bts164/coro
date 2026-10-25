# Future and Stream

## Overview

`Future` and `Stream` are the two foundational concepts of the library, mirroring Rust's
`Future` and `Stream` traits. They are C++20 **concepts** — structural interfaces enforced
at compile time — rather than base classes. Any type that satisfies the concept can be
`co_await`ed or iterated without inheriting from anything.

## Future

A `Future<T>` represents a single asynchronous value of type `T`. It is driven to completion
by the executor calling `poll()` repeatedly until the result is ready.

### Concept requirements

```cpp
template<typename F>
concept Future = requires(F& f, detail::Context& ctx) {
    typename F::OutputType;
    { F(std::move(f)) };
    { f.poll(ctx) } -> std::same_as<PollResult<typename F::OutputType>>;
};
```

- `F::OutputType` — the type produced on successful completion (`void` is allowed).
- `F(std::move(f))` — the future can be moved, which coro needs before the first poll (see
  below).
- `poll(ctx)` — advances the future. Returns one of the `PollResult` states described below.
  The future registers a waker via `ctx.getWaker()` when it returns `Pending` so the executor
  knows to re-poll it later.

### Movable until the first poll, pinned after

A future may be moved until its first `poll()` and must stay in place from then on, since
a started future may have handed its own address to a waker, a callback or a list.

Coro requires the move because it moves futures before they start: out of the function that
makes one, into `spawn()`, `select()` and the other combinators, and into the
`FutureAwaitable` that every `co_await` builds. The pin is a convention, not enforced:
deleting the move constructor would forbid those moves too, and C++ has no way to allow
moves only until a given point, as Rust's `Pin` does. A hand-written future stays safe to
move by doing nothing address-dependent before its first poll.

Coro's API tries to leave users no way to move a future once it has started, and most uses
leave none: a future handed to `co_await`, `spawn()` or a combinator is moved once, before it
starts, and is then out of the user's reach. The few exceptions are pointed out where they
occur. `coro::ref(f)` is one: it polls `f` in place, so `f` stays a named local the user could
still move or await directly after it has started.

This is coro's restriction, not the compiler's. C++20 builds the awaiter that
`await_transform` returns in place, so an awaiter with no copy or move constructor compiles.
The move comes from `FutureAwaitable`, which takes the future by value.

### PollResult states

`PollResult<T>` carries one of four states:

| State | Meaning |
|---|---|
| `Pending` | Not ready. The waker from `ctx` has been stored; the executor will re-poll when it fires. |
| `Ready(T)` | Completed successfully. The value is accessible via `.value()`. |
| `Error` | Faulted. An exception was captured; accessible via `.error()` / `.rethrowIfError()`. |
| `Dropped` | Cancelled and fully drained. Propagates upward through the coroutine stack. |

Construct return values with the corresponding helpers:

```cpp
return PollPending;           // Pending
return some_value;            // Ready — implicit conversion from T
return PollError(eptr);       // Error — wraps a std::exception_ptr
return PollDropped;           // Dropped
```

### Writing a custom Future

!!! warning "Your responsibility, like Rust's `unsafe`"
    Prefer coroutines and coro's combinators. Writing a future, or calling `poll()`
    yourself, is allowed but is coro's equivalent of `unsafe`: the API no longer keeps
    you away from the hazards, and you take on every contract rule on this page, pinning
    above all.

The canonical pattern: check for a result, return it if ready, otherwise store the waker
and return `Pending`. All shared state must be protected under a mutex.

```cpp
template<typename T>
class OneshotFuture {
public:
    using OutputType = T;

    PollResult<T> poll(detail::Context& ctx) {
        std::lock_guard lock(m_mutex);
        if (m_result.has_value()) {
            auto result = std::move(*m_result);
            if (result.has_value())
                return std::move(result.value());
            else
                return PollError(result.error());
        }
        m_waker = ctx.getWaker();
        return PollPending;
    }

    void set(T value) {
        std::shared_ptr<detail::Waker> w;
        {
            std::lock_guard lock(m_mutex);
            m_result.emplace(std::move(value));
            w = m_waker;
        }
        if (w) w->wake();
    }

    void set_error(std::exception_ptr e) {
        std::shared_ptr<detail::Waker> w;
        {
            std::lock_guard lock(m_mutex);
            m_result.emplace(std::unexpected(std::move(e)));
            w = m_waker;
        }
        if (w) w->wake();
    }

private:
    std::mutex m_mutex;
    std::optional<std::expected<T, std::exception_ptr>> m_result;
    std::shared_ptr<detail::Waker> m_waker;
};
```

Notes:

- The waker is read and cleared under the lock — this prevents a missed-wakeup race where
  `set()` reads a null waker just before `poll()` stores a new one.
- If the waker may be stored across threads (e.g. a callback run by another Runtime's I/O driver),
  use `std::atomic<std::shared_ptr<detail::Waker>>` instead.

### Usage

`co_await` drives the future automatically. Exceptions propagate as normal C++ exceptions:

```cpp
coro::Coro<void> example(OneshotFuture<int>& f) {
    try {
        int value = co_await f;
        std::cout << "received: " << value << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
    }
}
```

## Stream

A `Stream<T>` is an asynchronous sequence of values, analogous to an async iterator. It
yields zero or more items and then signals exhaustion. This mirrors Rust's `Stream` trait.

### Concept requirements

```cpp
template<typename T>
using StreamItem = std::conditional_t<std::is_void_v<T>, bool, std::optional<T>>;

template<typename S>
concept Stream = requires(S& s, detail::Context& ctx) {
    typename S::ItemType;
    { s.poll_next(ctx) } -> std::same_as<PollResult<detail::StreamItem<typename S::ItemType>>>;
};
```

- `S::ItemType` — the element type of the stream.
- `poll_next(ctx)` — advances the stream by one item. Returns:
    - `Ready(some(T))` — next item is available
    - `Ready(nullopt)` — stream is exhausted (see below)
    - `Pending` — no item ready yet; waker registered for later wake-up
    - `Error` — the stream itself failed; exception captured in the `PollResult`
    - `Dropped` — the stream was cancelled
- For a `void` stream, `std::optional<void>` is ill-formed, so `poll_next()` returns
  `bool` instead: `true` for an item, `false` for exhausted.

### Errors and exhaustion

- **An `Error` is a failure of the stream itself,** such as a broken socket. A failure
  that belongs to one item is part of that item: the stream yields it as an ordinary
  value and carries on. A `JoinSet` is the example: a task that threw is a
  `JoinResult` holding its exception, and the set never returns an `Error`.
- **Once failed, a stream stays failed.** Every later `poll_next()` returns an `Error`
  too. Which exception it carries is unspecified: the original, or another that
  describes the failed state. A consumer that catches a stream's error must not expect
  more items from it.
- **Exhausted is not an error, and not necessarily permanent.** `Ready(nullopt)` (or
  `false`) says no item is coming under the present conditions. Each stream's own
  contract says whether it can yield again: a `JoinSet` yields again after another
  `spawn()`, while an `MpscReceiver` whose senders are all gone never does.
- **`Dropped`** comes from a `Cancellable` stream that was cancelled, such as a
  `CoroStream` after its `cancel()`. It yields nothing more.

!!! warning "FIXME: `SignalStream` doesn't stay failed"
    After a failed read of its signal pipe, `SignalStream::poll_next()` returns the
    error once, and the next poll tries the read again. It should keep returning an
    error.

### Pinned from the first poll

A stream follows the futures' rule (see
[Movable until the first poll, pinned after](#movable-until-the-first-poll-pinned-after)): it
may be moved until its first `poll_next()` and must stay in place from then on, even between
items.

That is stricter than a stream strictly needs. It would be enough not to move a stream while a
poll is in flight, but from the caller's side it is not always obvious when one is, and a rule
that turns on that is too easy to break by accident.

Unlike a future, a stream is usually reached through a borrow. `next(s)` and
`blocking_next(s)` poll `s` in place, so `s` stays a named local the user could move after it
has started: handing a receiver to another task once it has been read from, or moving a
`JoinSet` into a member after its first `next()`. Move a stream to where it will live before
reading from it.

### Writing a custom Stream

!!! warning "Your responsibility, like Rust's `unsafe`"
    As for futures: prefer `CoroStream` and coro's own streams. Writing a stream, or
    calling `poll_next()` yourself, makes every contract rule on this page yours to keep.

```cpp
template<typename T>
class ChannelStream {
public:
    using ItemType = T;

    PollResult<std::optional<T>> poll_next(detail::Context& ctx) {
        std::lock_guard lock(m_mutex);
        if (m_error) {
            return PollError(m_error);  // kept: a failed stream stays failed
        }
        if (!m_queue.empty()) {
            T value = std::move(m_queue.front());
            m_queue.pop();
            return std::optional<T>(std::move(value));
        }
        if (m_closed) {
            return std::optional<T>(std::nullopt);  // exhausted
        }
        m_waker = ctx.getWaker();
        return PollPending;
    }

    void send(T value) {
        std::shared_ptr<detail::Waker> w;
        {
            std::lock_guard lock(m_mutex);
            m_queue.push(std::move(value));
            w = m_waker;
        }
        if (w) w->wake();
    }

    void close() {
        std::shared_ptr<detail::Waker> w;
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
            w = m_waker;
        }
        if (w) w->wake();
    }

private:
    std::mutex m_mutex;
    std::queue<T> m_queue;
    std::exception_ptr m_error;
    std::shared_ptr<detail::Waker> m_waker;
    bool m_closed = false;
};
```

### Usage

`poll_next` is not called directly. The `next()` helper wraps a `Stream` into a
`Future<std::optional<T>>` that can be `co_await`ed in a loop:

```cpp
coro::Coro<void> consumer(ChannelStream<int>& stream) {
    try {
        while (auto item = co_await coro::next(stream)) {
            std::cout << "received: " << *item << "\n";
        }
        // Ready(nullopt) — stream exhausted normally
    } catch (const std::exception& e) {
        std::cerr << "stream error: " << e.what() << "\n";
    }
}
```

## Design notes

### Context and Waker are separate types

`Context` is ephemeral — it lives on the call stack only for the duration of `poll()`.
Futures that need to wake the executor after `poll()` returns (e.g. from an I/O callback)
must extract and store the `Waker` via `ctx.getWaker()`. They cannot hold a pointer to
`Context` itself.

`Waker` is an abstract base with `virtual void wake()` and `virtual shared_ptr<Waker> clone()`.
The executor provides a concrete implementation. Futures store a `shared_ptr<Waker>`.

### Spurious wakes

The poll contract explicitly allows futures to call `waker->wake()` even when they are not
yet ready. Executors must tolerate this by simply re-polling: a future that returns `Pending`
again is always correct, though potentially wasteful.

C++20 coroutines require special handling here. When a coroutine is resumed after a spurious
wake, `await_resume()` would be called on a future that is still `Pending` — calling `.value()`
on a `Pending` `PollResult` is undefined behaviour. The library handles this with an
`m_poll_current` hook on the promise: before resuming the coroutine, `Coro<T>::poll()` re-polls
the inner future. If it is still `Pending`, the resume is suppressed and `Pending` is returned
to the executor instead. See [task_and_executor.md](task_and_executor.md) for details.
