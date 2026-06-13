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
concept Future = requires(F f, detail::Context& ctx) {
    typename F::OutputType;
    { f.poll(ctx) } -> std::same_as<PollResult<typename F::OutputType>>;
};
```

- `F::OutputType` — the type produced on successful completion (`void` is allowed).
- `poll(ctx)` — advances the future. Returns one of the `PollResult` states described below.
  The future registers a waker via `ctx.getWaker()` when it returns `Pending` so the executor
  knows to re-poll it later.

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
- If the waker may be stored across threads (e.g. an I/O callback on the libuv thread),
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
template<typename S>
concept Stream = requires(S s, detail::Context& ctx) {
    typename S::ItemType;
    { s.poll_next(ctx) } -> std::same_as<PollResult<std::optional<typename S::ItemType>>>;
};
```

- `S::ItemType` — the element type of the stream.
- `poll_next(ctx)` — advances the stream by one item. Returns:
  - `Ready(some(T))` — next item is available
  - `Ready(nullopt)` — stream is exhausted (no more items will follow)
  - `Pending` — no item ready yet; waker registered for later wake-up
  - `Error` — stream faulted; exception captured in the `PollResult`
  - `Dropped` — stream was cancelled

### Writing a custom Stream

```cpp
template<typename T>
class ChannelStream {
public:
    using ItemType = T;

    PollResult<std::optional<T>> poll_next(detail::Context& ctx) {
        std::lock_guard lock(m_mutex);
        if (m_error) {
            return PollError(std::exchange(m_error, nullptr));
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
