# Future and Stream

## Overview

Future and Stream are the foundation for polling the readiness of an asynchronous operation. They follow
as close as possible the Future and Stream traits un Rust, but are instead implemented as C++ concepts

## Requirements

- Should follow as close as possible the interface of Rust's Future and Stream traits
- Should be C++ concepts defining an interface and not require inheritence from a base class
- A `PollResult<T>` template class similar to Rust's used to return the state of the future or stream
     - Should be similar to std::optional and may even privatly inherit from it (though it doesn't have to)
     - Conversion to/from std::optional should be explicit if it's allowed at all
     - Unlike std::optional needs to handle void as the template types
- Future class F requires a type F::OutputType defining the result type of the future
- Main poll operation `PollResult<F::OutputType> f.poll(Context &ctx)`
- Must properly handle exceptions thrown by `poll` (i.e. catch and save the exception in the `PollResult`)
- Stream class S requires a type `S::ItemType` defining the element type of the stream
- Main poll operation `PollResult<std::optional<S::ItemType>> s.poll_next(Context &ctx)`
     - `Ready(some(T))` — next item is available
     - `Ready(nullopt)` — stream is exhausted (no more items)
     - `Pending` — no item ready yet, waker registered for later wake-up
     - `Error` — stream faulted; exception captured in `PollResult`
- A `.next()` helper adapts a `Stream` into a `Future<std::optional<T>>` so it can be `co_await`ed in a loop

## Future

### Interface example

```cpp
template<typename T>
class OneshotFuture
{
public:
     using OutputType = T;
     PollResult<T> poll(Context &ctx)
     {
          std::unique_lock lk(m_mutex);
          if (m_value.has_value()) {
               return std::move(m_value).value();
          }
          if (nullptr != m_except) {
               return PollError(std::exchange(m_except, nullptr));
          }
          m_waker = ctx.getWaker();
          return PollPending;
     }
     void set(T x)
     {
          std::unique_lock(m_mutex);
          m_value.emplace(std::move(x));
          if (nullptr != m_waker) {
               std::exchange(m_waker, nullptr)->wake();
          }
     }
     bool setError(std::exception_ptr e)
     {
          std::unique_lock(m_mutex);
          m_error = PollError(e);
          if (nullptr != m_waker) {
               std::exchange(m_waker, nullptr)->wake();
          }
     }
private:
     std::mutex m_mutex;
     std::optional<T> m_value;
     std::exception_ptr m_except;
     std::shared_ptr<Waker> m_waker;
};
```

### Usage Example

```cpp
Oneshot<int> value;

Coroutine<void> foo()
{
     ...
     try {
          int value = co_await value;
          std::cout << "bar sent value = " << v << "\n";
     } catch (std::exception const &e) {
          std::cerr << "bar threw exception " << e.what() << "\n";
     }
     ...
}

Coroutine<void> bar()
{
     ...
     value.set(3);
     // or 
     try (
          ...
     ) catch (...) {
          value.setError(std::current_exception());
     }
     ...
}
```
## Stream

A `Stream` is an asynchronous sequence of values, analogous to an async iterator. It mirrors
Rust's `Stream` trait. Unlike a `Future` which resolves once, a `Stream` yields zero or more
items before signaling exhaustion.

### Interface example

```cpp
template<typename T>
class ChannelStream
{
public:
    using ItemType = T;

    // Returns Ready(some(T)) for the next item, Ready(nullopt) when exhausted, or Pending
    PollResult<std::optional<T>> poll_next(Context &ctx)
    {
        std::unique_lock lk(m_mutex);
        if (m_except) {
            return PollError(std::exchange(m_except, nullptr));
        }
        if (!m_queue.empty()) {
            T value = std::move(m_queue.front());
            m_queue.pop();
            return std::optional<T>(std::move(value));
        }
        if (m_closed) {
            return std::optional<T>(std::nullopt); // stream exhausted
        }
        m_waker = ctx.getWaker();
        return PollPending;
    }

    void send(T value)
    {
        std::unique_lock lk(m_mutex);
        m_queue.push(std::move(value));
        if (m_waker) {
            std::exchange(m_waker, nullptr)->wake();
        }
    }

    void close()
    {
        std::unique_lock lk(m_mutex);
        m_closed = true;
        if (m_waker) {
            std::exchange(m_waker, nullptr)->wake();
        }
    }

    void setError(std::exception_ptr e)
    {
        std::unique_lock lk(m_mutex);
        m_except = e;
        m_closed = true;
        if (m_waker) {
            std::exchange(m_waker, nullptr)->wake();
        }
    }

private:
    std::mutex m_mutex;
    std::queue<T> m_queue;
    std::exception_ptr m_except;
    std::shared_ptr<Waker> m_waker;
    bool m_closed = false;
};
```

### Usage Example

`poll_next` would not typically be called directly. Instead a `.next()` helper wraps `poll_next`
into a `Future<std::optional<T>>`, which can be `co_await`ed in a loop:

```cpp
ChannelStream<int> stream;

Coroutine<void> consumer()
{
    ...
    try {
        while (auto item = co_await stream.next()) {
            std::cout << "received: " << *item << "\n";
        }
        // Ready(nullopt) — stream exhausted normally
    } catch (std::exception const &e) {
        std::cerr << "stream error: " << e.what() << "\n";
    }
    ...
}

Coroutine<void> producer()
{
    ...
    stream.send(1);
    stream.send(2);
    stream.send(3);
    stream.close();
    ...
}
```

## Design Considerations

### Waker and Context separation

`Context` and `Waker` are kept as separate types for lifetime reasons. Futures that register
callbacks (e.g. with libuv) need to *store* the waker so the I/O reactor can call `wake()` later,
after `poll` has returned. `Context` is ephemeral — it lives on the call stack only for the
duration of `poll` — so a future cannot safely hold a pointer to it beyond that call.

**Design:**
- `Waker` is an abstract base class with `virtual void wake()` and `virtual shared_ptr<Waker> clone()`
- `Context` holds a `shared_ptr<Waker>` plus any other per-poll state
- Concrete executor contexts may subclass `Context` to add extra fields
- Futures store a `shared_ptr<Waker>` when they need to defer waking (e.g. after async I/O)

### PollResult states

`PollResult<T>` has three states:
- `Pending` — the future is not yet ready
- `Ready(T)` — the future completed with a value (supports `void`)
- `Error` — the future threw an exception; the exception is captured and can be re-thrown later

### Spurious wakes and co_await correctness

The poll contract (inherited from Rust) explicitly allows futures to fire their waker even when
they are not yet ready — a *spurious wake*. Executors must tolerate this: re-polling a future
that returns `Pending` again is always correct, though potentially wasteful.

Coroutines introduce a subtle asymmetry that makes spurious wakes dangerous if not handled
explicitly. In Rust, the compiler lowers `expr.await` into a `loop { match poll(...) { Pending
=> return Pending, Ready(v) => break v } }` — each time the outer future is polled, the inner
future is polled from the top of that loop. In C++20, `co_await` is lowered differently:
`await_ready` / `await_suspend` execute once at the suspension point; when the coroutine is
later **resumed**, control jumps directly to `await_resume`. There is no automatic re-poll of
the inner future on resume.

**The failure mode:** without a guard, a spurious wake causes the executor to call
`handle.resume()` on the outer coroutine, which lands in `FutureAwaitable::await_resume()`.
If the inner future is still `Pending`, `await_resume()` has no valid value to return — calling
`.value()` on a `Pending` `PollResult` is undefined behaviour, and the coroutine continues past
the `co_await` with garbage data.

**The fix — `m_poll_current` hook:** a `std::function<bool()>` field on the promise acts as a
re-poll gate. `FutureAwaitable::await_suspend()` stores a lambda there that re-polls the inner
future using the promise's current `Context*`. `Coro<T>::poll()` and `CoroStream::poll_next()`
check this hook *before* calling `handle.resume()`:

```
Coro::poll(ctx):
    promise.m_ctx = &ctx          // update Context for current tick

    if m_poll_current:
        if not m_poll_current():  // re-poll with fresh context
            return Pending        // still not ready — absorb the spurious wake
        m_poll_current = null     // ready — clear hook before resuming

    handle.resume()               // safe: inner future is confirmed non-Pending
    ...
```

Key properties of this design:

- **No spurious resumes.** The coroutine is only resumed when the inner future is confirmed
  non-Pending. `await_resume()` can always call `.value()` safely.
- **Waker re-registration.** Each call to `m_poll_current()` passes the fresh `Context`,
  so the inner future re-registers the waker for the current executor tick.
- **Yield points are unaffected.** `m_poll_current` is only set by `await_suspend()`, which
  is not called on `co_yield`. When the coroutine is suspended at a yield point, the hook is
  null and `poll_next()` resumes directly.
- **Cleared on each new `co_await`.** `await_transform()` sets `m_poll_current = nullptr`
  before returning the new `FutureAwaitable`, so a stale hook from a previous suspension can
  never interfere with a subsequent one.
