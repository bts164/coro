# libuv Integration

## Overview

The library uses libuv as its I/O reactor. A dedicated `IoService` class owns a
`uv_loop_t` and runs it on a background thread. Worker threads submit requests to it
via a thread-safe queue, and libuv callbacks call `waker->wake()` when events fire,
routing tasks back into the executor via the standard `enqueue()` injection path.

`IoService` backs `sleep_for()` / `SleepFuture` today and serves as the foundation for
all async I/O primitives in `include/coro/io/` (`TcpStream`, `WsStream`, `WsListener`).
`Waker`, `Context`, and `Executor` are unchanged — leaf futures call `ctx.getWaker()`,
store it, and the I/O callback fires it when the event arrives.

---

## The Core Problem: libuv Is Not Thread-Safe

Nearly all libuv API calls (`uv_timer_start`, `uv_read_start`, `uv_tcp_connect`, etc.)
**must be made from the thread that owns the event loop**. Worker threads that poll tasks
cannot call these APIs directly.

The solution is to have a **dedicated I/O thread** that owns the loop. Worker threads
submit registration requests to this thread through a thread-safe queue, and the I/O
thread processes them. When events fire, libuv callbacks run on the I/O thread and call
`waker->wake()`, which routes the task back into the executor's work-stealing queues via
the existing thread-safe `enqueue()` path.

---

## The Mechanism: `uv_async_t` as a Cross-Thread Doorbell

`uv_async_t` is the **only** truly thread-safe libuv primitive. Calling
`uv_async_send(&async)` from any thread wakes the loop and schedules a callback to fire
on the loop thread. Critically, multiple sends before the callback fires are
**coalesced** — the callback fires at least once but not necessarily once per send. The
callback must therefore drain an entire queue rather than assuming one send = one request.

### Full request flow

```
Worker thread                         I/O thread (owns uv_loop)
─────────────────────────────         ──────────────────────────────────────
SleepFuture::poll():
  1. allocate shared_ptr<TimerState>
  2. submit StartTimer{state*, deadline}
     push to m_io_queue (mutex)
     uv_async_send(&m_async)  ────────► io_async_cb fires:
                                          process_queue() drains m_io_queue:
                                            uv_timer_init(loop, &state->handle)
                                            state->handle.data = state   // back-pointer
                                            uv_timer_start(&state->handle,
                                              timer_cb, delay_ms, 0)
                                          (loop blocks in uv_run)

                                          ... deadline expires ...
                                          timer_cb(uv_timer_t* h):
                                            state = (TimerState*)h->data
                                            if (!state->fired.exchange(true))
                                              state->waker.load()->wake()
                                              uv_close(h, close_cb)
                                                                ──────────────► executor->enqueue(task)
                                          close_cb(uv_handle_t* h):            task re-polled
                                            delete (TimerState*)h->data
```

---

## `IoService` Class

Rather than embedding the I/O loop fields directly in `Runtime`, the I/O driver lives in
its own class `IoService` (in `include/coro/runtime/io_service.h`). This mirrors the
existing `TimerService` structure and keeps `Runtime` lean.

```cpp
// include/coro/runtime/io_service.h
class IoService {
public:
    IoService();
    ~IoService();

    IoService(const IoService&)            = delete;
    IoService& operator=(const IoService&) = delete;

    /// Thread-safe. Pushes req onto the queue and signals the I/O thread.
    void submit(std::unique_ptr<IoRequest> req);

    /// Signals the I/O thread to stop and joins it. Safe to call multiple times.
    void stop();

private:
    void io_thread_loop();
    void process_queue();   // called from io_async_cb; drains m_io_queue

    uv_loop_t  m_uv_loop;
    uv_async_t m_async;        // cross-thread doorbell
    std::thread m_io_thread;

    std::mutex                          m_io_queue_mutex;
    std::deque<std::unique_ptr<IoRequest>> m_io_queue;
    std::atomic<bool>                   m_stopping{false};
};

/// Sets the thread-local current IoService. Called by Runtime::block_on() and worker threads.
void set_current_io_service(IoService* svc);

/// Returns the thread-local IoService, or throws if called outside a Runtime context.
IoService& current_io_service();
```

The `TimerService` class and `set_current_timer_service` / `schedule_wakeup` free
functions that were used in earlier iterations have been removed. `SleepFuture::poll()`
now calls `current_io_service().submit(StartRequest{...})` directly.

---

## `IoRequest` — Polymorphic Command

`IoRequest` is an abstract base class following the **command pattern**. Each subclass
encapsulates the libuv-specific logic for one operation and executes it on the I/O thread
by overriding `execute()`.

```cpp
// include/coro/runtime/io_service.h

struct IoRequest {
    virtual ~IoRequest() = default;
    /// Called on the I/O thread with exclusive access to the uv_loop.
    virtual void execute(uv_loop_t* loop) = 0;
};
```

`process_queue()` on the I/O thread is trivially simple — it knows nothing about
individual operation types:

```cpp
void IoService::process_queue() {
    std::deque<std::unique_ptr<IoRequest>> local;
    { std::lock_guard lk(m_io_queue_mutex); std::swap(local, m_io_queue); }
    for (auto& req : local)
        req->execute(&m_uv_loop);
}
```

Follow-on I/O primitives (`TcpConnect`, `TcpRead`, `UdpSend`, `FileRead`, etc.) each add
a new `IoRequest` subclass without modifying `IoService` or `process_queue()` at all.

### I/O operation ownership model

**`IoService` is kept completely ignorant of every concrete operation type.** All
operation-specific state structs, request types, and libuv callbacks are private
implementation details of the Future that owns them — not shared types in any header.

The rule for where to define them:

- **Private nested types** when one Future owns all the request types. `SleepFuture`
  is the only type that ever constructs a `StartRequest` or `CancelRequest`, so they
  are private nested structs of `SleepFuture`. libuv callbacks (`timer_cb`, `close_cb`)
  are private `static` methods of the same class. No other header needs to know they exist.

- **`coro::detail::<op>` namespace** when two or more sibling Futures need to share
  state — for example, a future TCP layer where a connect future, a read future, and a
  write future all share the same `uv_tcp_t` handle. Nesting in either sibling would be
  arbitrary; a dedicated detail namespace groups them without implying ownership. `friend`
  declarations across unrelated types are avoided.

This keeps `io_service.h` minimal (only `IoRequest`, `IoService`, and the thread-local
accessors) and makes each I/O subsystem fully self-contained.

---

## Timer Handle Lifecycle

Each `SleepFuture` instance manages exactly one `uv_timer_t` heap allocation:

```
State            Who owns the handle
─────────────    ────────────────────────────────────────────────────────────
Pending          SleepFuture holds shared_ptr<State>; has submitted StartRequest
Fired            timer_cb running on I/O thread; calls wake() then uv_close()
Closing          libuv closing asynchronously; close_cb will free it
Cancelled        SleepFuture destructor submitted CancelRequest before firing
Closed           close_cb ran; handle freed; nothing holds the pointer
```

`SleepFuture::State` is allocated on the heap and shared between `SleepFuture` and the
I/O thread via `shared_ptr`. `CancelRequest` also holds a `shared_ptr<State>` so the
state stays alive until the I/O thread processes the request, regardless of when
`SleepFuture` is destroyed.

**Double-close prevention** — both `timer_cb` and `CancelRequest::execute()` claim the
`uv_close` call via an atomic exchange on `fired`. Whichever side wins `fired.exchange(true)`
first owns the close; the other side is a safe no-op:

```
timer_cb (I/O thread):
    if (!state->fired.exchange(true))  // wins → owns uv_close
        waker->wake()
        uv_close(&state->handle, close_cb)

CancelRequest::execute() (I/O thread):
    if (!state->fired.exchange(true))  // wins → owns uv_close
        uv_timer_stop(&state->handle)
        uv_close(&state->handle, close_cb)
```

`close_cb` deletes the heap-allocated `shared_ptr<State>` wrapper stored in
`handle->data`, dropping the last reference and freeing the state.

---

## `SleepFuture` Design

All timer-specific types are private to `SleepFuture`. Nothing outside `sleep.h` needs
to know they exist — `IoService` in particular has zero knowledge of timers.

```cpp
// include/coro/sync/sleep.h

class SleepFuture {
public:
    using OutputType = void;
    // ... constructor, destructor, poll() ...

private:
    // Shared between the worker thread (poll()) and the I/O thread (timer_cb).
    struct State : std::enable_shared_from_this<State> {
        uv_timer_t                                  handle;  // must be first field
        std::atomic<std::shared_ptr<detail::Waker>> waker;   // RACE: written by worker, read by I/O thread
        std::atomic<bool>                           fired{false};
    };

    // Arms the one-shot timer on the I/O thread.
    struct StartRequest : IoRequest {
        std::shared_ptr<State>                 state;
        std::chrono::steady_clock::time_point  deadline;
        void execute(uv_loop_t* loop) override;
    };

    // Cancels and closes the timer handle on the I/O thread.
    struct CancelRequest : IoRequest {
        std::shared_ptr<State> state;
        void execute(uv_loop_t* loop) override;
    };

    // libuv callbacks — static to satisfy C function-pointer ABI.
    // Both run exclusively on the I/O thread.
    static void timer_cb(uv_timer_t* handle);
    static void close_cb(uv_handle_t* handle);

    std::chrono::steady_clock::time_point m_deadline;
    std::shared_ptr<State>                m_state;        // null until first poll()
    IoService*                            m_io_service = nullptr;  // cached on first poll()
};
```

`StartRequest` and `CancelRequest` are private nested structs of `SleepFuture`, so they
have full access to `SleepFuture`'s private members (including `State`) without any
`friend` declarations. The callbacks are private `static` methods — they satisfy the C
function-pointer ABI required by libuv while remaining scoped to `SleepFuture`.

### Waker concurrency

`timer_cb` on the I/O thread reads `state->waker` while a re-polled `SleepFuture` on a
worker thread may be writing it. `State::waker` is therefore
`std::atomic<std::shared_ptr<detail::Waker>>` (C++20 specialisation), making the store in
`poll()` and the load in `timer_cb` race-free without a mutex.

### Timer resolution

libuv timers have **millisecond resolution**. `StartRequest::execute()` converts the
remaining duration to milliseconds using `std::chrono::ceil<milliseconds>` (rounding
**up**), so the timer never fires before the deadline. A sub-millisecond remainder is
always rounded up to the next whole millisecond.

> **Rounding caveat:** because the delay is re-derived from `steady_clock::now()` at the
> moment `execute()` runs on the I/O thread (not at `submit()` time), any queuing latency
> between the worker thread calling `submit()` and the I/O thread processing the request
> shortens the effective remaining duration before the ceil. In the worst case the timer
> still fires at or after the original deadline, but callers must not assume it fires
> exactly at `deadline` — actual wakeup will typically be 0–2 ms late due to OS scheduling
> and the round-trip through the I/O thread.

In practice this is not a meaningful limitation: the round-trip through the executor and
I/O thread already adds latency on the order of microseconds to milliseconds, so
sub-millisecond timer precision is not achievable regardless.

---

## Shutdown Protocol

```
Runtime::~Runtime():
  1. m_executor.reset()          // joins all worker threads; no more waker->wake() calls
  2. m_io_service.stop()         // signals I/O thread and joins it

IoService::stop():
  1. { lock } m_stopping = true
  2. uv_async_send(&m_async)     // wake loop one last time
  3. m_io_thread.join()

io_async_cb (I/O thread):
  process_queue()                // drain any last requests
  if (m_stopping):
    uv_close(&m_async, nullptr)  // removing the last ref-counted handle lets uv_run() return

// uv_run() returns → io_thread_loop() exits → join() completes.
```

After `uv_run` returns, `uv_loop_close(&m_uv_loop)` is called. If any `uv_timer_t`
handles were not yet closed (e.g. in-flight cancellations that arrived after `stop()` was
called), `uv_loop_close` will return `UV_EBUSY`. In that case the implementation must call
`uv_run` one more time in `UV_RUN_DEFAULT` mode (with `m_stopping` set) to flush
remaining close callbacks before calling `uv_loop_close` again.

### Declaration order in `Runtime`

`m_io_service` must be declared **before** `m_executor` so that it is destroyed
**after** the executor (C++ destroys members in reverse declaration order). This guarantees
no `waker->wake()` arrives on the I/O thread after the loop has been closed.

```cpp
class Runtime {
    IoService                 m_io_service;  // destroyed last
    std::unique_ptr<Executor> m_executor;    // destroyed first
};
```

---

## Why a Dedicated I/O Thread Instead of `UV_RUN_NOWAIT`

An alternative design drives the event loop inline — calling
`uv_run(loop, UV_RUN_NOWAIT)` from a worker thread between task polls. This avoids a
separate thread but has several drawbacks:

- `uv_run` must still be called by the loop's owner thread. With work-stealing, any
  worker can end up calling it, which requires strict ownership tracking.
- `UV_RUN_NOWAIT` is inherently polling — it only processes callbacks that are already
  ready. Timers that expire while no tasks are running still require the loop to tick,
  which needs a dedicated caller.
- `UV_RUN_DEFAULT` with a dedicated thread lets the OS park the I/O thread at the kernel
  level between events (epoll/kqueue wait). `UV_RUN_NOWAIT` misses this entirely.

A dedicated I/O thread is simpler, matches how Tokio structures its I/O driver, and
cleanly separates scheduling concerns from I/O concerns.

---

## Cancellation

When a `SleepFuture` or I/O future is destroyed mid-suspension (e.g. the task is
cancelled or the losing branch of a `select` is dropped), it must cancel its pending
libuv registration. Since `uv_timer_stop()` and `uv_close()` must also be called on
the loop thread, the future's destructor pushes a `CancelTimer` onto the queue and
calls `uv_async_send()` via `IoService::submit()`. The I/O thread processes the
cancellation and closes the handle.

The destructor always submits `CancelTimer` (if `m_state` is set). `CancelTimer::execute()`
uses `fired.exchange(true)` to atomically claim the `uv_close` call. If the timer already
fired, `timer_cb` will have won the exchange and `CancelTimer` is a no-op. This is simpler
than a conditional check in the destructor and eliminates the TOCTOU window that a
`fired.load()` check would leave open.

---

## Relationship to other abstractions

| Abstraction | Notes |
|---|---|
| `Waker` / `Context` | Unchanged |
| `Executor::enqueue()` | Unchanged — I/O callbacks wake tasks via the normal injection path |
| `SleepFuture` | Submits `StartRequest`/`CancelRequest` to `IoService`; destructor submits cancel |
| `Runtime` | Owns `m_io_service`; calls `set_current_io_service` in `block_on` and worker thread startup |
| All combinators, `spawn`, `JoinHandle` | Unchanged |

---

## I/O Primitives

`include/coro/io/` contains async wrappers built on `IoService`. Each follows the same
pattern as `SleepFuture`: allocate a handle state struct, submit an `IoRequest`, store the
`Waker`, and cancel in the destructor if the operation has not yet completed.

**Implemented:**
- **`TcpStream`** — async TCP connection; `connect()`, `read()`, `write()`
- **`WsStream`** — async WebSocket client; `connect()`, `send()`, `receive()`
- **`WsListener`** — async WebSocket server; `bind()`, `accept()`

**Planned** (see `roadmap.md`):
- `TcpListener` — async TCP accept loop
- `UdpSocket` — async send/recv
- `File` — async read/write via libuv's thread-pool file I/O
- DNS resolution

---

## Race Conditions

Known concurrency concerns and how they are resolved:

**`State::waker` concurrent read/write** — worker thread writes `waker` on re-poll; I/O
thread reads it in `timer_cb`. Resolved by `std::atomic<std::shared_ptr<Waker>>` in `State`.

**Double-close of `uv_timer_t`** — `timer_cb` and `CancelRequest::execute()` both call
`uv_close`. Resolved by `fired.exchange(true)` — the first caller owns the close, the other
is a safe no-op.

**`State` freed before `CancelRequest` executes** — if `SleepFuture` is destroyed before
the I/O thread processes `CancelRequest`, the state must remain alive. Resolved by
`CancelRequest` holding `shared_ptr<State>`.

**`m_stopping` read/write race** — `io_async_cb` reads `m_stopping` outside the lock while
`stop()` writes it. Resolved by `std::atomic<bool> m_stopping`.

**`State` is not standard-layout** — `State` contains `std::atomic<std::shared_ptr<>>`,
making first-field pointer casts UB. Resolved by storing a heap-allocated
`shared_ptr<State>` wrapper in `handle->data` and recovering it in callbacks via
`static_cast<std::shared_ptr<State>*>(handle->data)`. `close_cb` deletes the wrapper,
decrementing the ref count.

**`uv_async_send` after `uv_close(&m_async)`** — calling `submit()` after shutdown is UB.
Prevented by the shutdown ordering: executor joins (no more `wake()` calls) before
`IoService::stop()` is called.

**Stray in-flight handles at `uv_loop_close` time** — if cancellation requests arrive
after the final `process_queue()` drain, `uv_loop_close` returns `UV_EBUSY`. Handled by
re-running `uv_run` one more time to flush remaining close callbacks.

---

## Dependencies

libuv is managed via Conan. Add the following to `conanfile.txt` (or `conanfile.py`):

```
[requires]
libuv/1.48.0
```

The CMake integration target is `libuv::libuv`. Link it to the `coro` library target:

```cmake
target_link_libraries(coro PUBLIC libuv::libuv)
```

---

## Initialization and Shutdown Sequence Diagram

```
Runtime ctor                 IoService ctor              I/O thread
────────────────             ──────────────────          ──────────────────────────────────
                             uv_loop_init(&m_uv_loop)
                             uv_async_init(&m_uv_loop,
                               &m_async, io_async_cb)
                             m_io_thread = thread(...)  → uv_run(&m_uv_loop, UV_RUN_DEFAULT)
m_executor = make(...)
set_current_io_service(...)  (worker threads also call
                              set_current_io_service)

... runtime running ...

Runtime dtor
m_executor.reset()           ← worker threads join
m_io_service.stop():
  m_stopping = true
  uv_async_send(&m_async)                               io_async_cb:
                                                          process_queue()  // drain last reqs
                                                          uv_close(&m_async, nullptr)
                                                        // loop exits when all handles closed
                                                        uv_run returns
m_io_thread.join()           ←─────────────────────────────────────────────
uv_loop_close(&m_uv_loop)
```
