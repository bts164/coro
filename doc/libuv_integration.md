# libuv Integration

## Overview

The library currently uses a `TimerService` — a dedicated background thread with a
`std::priority_queue` of `(deadline, Waker)` pairs — as a temporary stand-in for real
timer support. `SleepFuture::poll()` calls `schedule_wakeup(deadline, waker)` which
posts to this queue. There is no I/O support yet.

The goal of libuv integration is to replace `TimerService` with a proper event loop and
add async I/O primitives (`TcpStream`, `File`, DNS, etc.) that compose naturally with the
rest of the library. The waker/context design already supports this — leaf futures call
`ctx.getWaker()` and store it for an external callback to fire. Nothing about `Waker` or
`Context` needs to change.

---

## Goals

- **Replace `TimerService`** with a libuv-backed I/O driver so timers are OS-precise
  (epoll/kqueue) rather than busy-polled.
- **Provide an `IoService` abstraction** that worker threads can reach via a thread-local,
  analogous to the existing `schedule_wakeup()` / `set_current_timer_service()` API.
- **Keep `Waker` / `Context` / `Executor` unchanged.** Only `SleepFuture` and `Runtime`
  change on the consuming side.
- **Lay the foundation for async I/O primitives** (`TcpStream`, `UdpSocket`, `File`, DNS)
  that will be added in a follow-on phase (`include/coro/io/`).
- **Correct shutdown ordering**: executor threads must join before the I/O loop closes.

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

The existing `set_current_timer_service` / `schedule_wakeup` free functions and
`TimerService` class are **removed**. Their only caller (`SleepFuture::poll`) is updated
to use `current_io_service().submit(StartTimer{...})` instead.

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

Concrete request types for this phase:

```cpp
struct StartTimer : IoRequest {
    TimerState*                            state;    // raw pointer; SleepFuture or CancelTimer keeps shared_ptr alive
    std::chrono::steady_clock::time_point  deadline;

    void execute(uv_loop_t* loop) override {
        uv_timer_init(loop, &state->handle);
        state->handle.data = state;  // back-pointer for timer_cb / close_cb; must be set before uv_timer_start
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        uv_timer_start(&state->handle, timer_cb, std::max<int64_t>(0, ms), 0);
    }
};

struct CancelTimer : IoRequest {
    std::shared_ptr<TimerState> state;  // keeps TimerState alive until I/O thread processes this

    void execute(uv_loop_t* /*loop*/) override {
        // Atomically claim the close. If timer_cb already set fired=true it owns uv_close;
        // we must not call it again.
        if (!state->fired.exchange(true)) {
            uv_timer_stop(&state->handle);
            uv_close(reinterpret_cast<uv_handle_t*>(&state->handle), close_cb);
        }
    }
};
```

The two static callbacks used by both request types:

```cpp
// Both run on the I/O thread.

static void timer_cb(uv_timer_t* handle) {
    auto* state = static_cast<TimerState*>(handle->data);
    if (!state->fired.exchange(true)) {
        state->waker.load()->wake();
        uv_close(reinterpret_cast<uv_handle_t*>(handle), close_cb);
    }
}

static void close_cb(uv_handle_t* handle) {
    // Decrement the shared_ptr ref count, freeing TimerState when it reaches zero.
    delete static_cast<TimerState*>(handle->data);
}
```

Follow-on I/O primitives (`TcpConnect`, `TcpRead`, `UdpSend`, `FileRead`, etc.) each add
a new `IoRequest` subclass in `include/coro/io/` without modifying `IoService` or
`process_queue()` at all.

---

## Timer Handle Lifecycle

Each `SleepFuture` instance manages exactly one `uv_timer_t` heap allocation:

```
State            Who owns the handle
─────────────    ────────────────────────────────────────────────────────────
Pending          SleepFuture holds the raw pointer; has submitted StartTimer
Fired            timer_cb running on I/O thread; calls wake() then uv_close()
Closing          libuv closing asynchronously; close_cb will free it
Cancelled        SleepFuture destructor submitted CancelTimer before firing
Closed           close_cb ran; handle freed; nothing holds the pointer
```

`TimerState` is allocated on the heap and shared between `SleepFuture` and the I/O thread:

```cpp
struct TimerState {
    uv_timer_t                                  handle;  // must be first field; address must be stable
    std::atomic<std::shared_ptr<detail::Waker>> waker;   // written by worker, read by I/O thread
    std::atomic<bool>                           fired{false};
};
```

`SleepFuture` holds a `std::shared_ptr<TimerState>`. `CancelTimer` also holds a
`shared_ptr<TimerState>` so the state stays alive until the I/O thread processes the
request, regardless of when `SleepFuture` is destroyed.

**Double-close prevention** — both `timer_cb` and `CancelTimer::execute()` claim the
`uv_close` call via an atomic exchange on `fired`. Whichever side wins `fired.exchange(true)`
first owns the close; the other side is a no-op:

```
timer_cb (I/O thread):
    if (!state->fired.exchange(true))  // wins → owns uv_close
        waker->wake()
        uv_close(&state->handle, close_cb)

CancelTimer::execute() (I/O thread):
    if (!state->fired.exchange(true))  // wins → owns uv_close
        uv_timer_stop(&state->handle)
        uv_close(&state->handle, close_cb)
```

`close_cb` releases the `shared_ptr<TimerState>` stored in `handle->data`, dropping the
last reference and freeing the state.

---

## Updated `SleepFuture` Design

```cpp
struct SleepFuture {
    using OutputType = void;

    std::chrono::steady_clock::time_point m_deadline;
    // Null until first poll(); set when we've registered with IoService.
    std::shared_ptr<TimerState> m_state;

    explicit SleepFuture(std::chrono::nanoseconds duration)
        : m_deadline(std::chrono::steady_clock::now() + duration) {}

    ~SleepFuture() {
        if (m_state) {
            // Pass the shared_ptr into CancelTimer so TimerState outlives this destructor.
            // CancelTimer::execute() uses fired.exchange(true) to avoid double-close if
            // the timer already fired before the I/O thread processes the cancel.
            current_io_service().submit(std::make_unique<CancelTimer>(m_state));
        }
    }

    PollResult<void> poll(detail::Context& ctx) {
        if (std::chrono::steady_clock::now() >= m_deadline)
            return PollReady;
        if (!m_state) {
            // First poll: register with the I/O driver.
            m_state = std::make_shared<TimerState>();
            m_state->waker = ctx.getWaker();
            current_io_service().submit(
                std::make_unique<StartTimer>(m_state.get(), m_deadline));
        } else {
            // Re-polled before timer fired (e.g. woken by another select branch).
            // Atomically update the waker — timer_cb may read it concurrently on the I/O thread.
            m_state->waker.store(ctx.getWaker());
        }
        return PollPending;
    }
};
```

### Waker concurrency

`timer_cb` on the I/O thread reads `state->waker` while a re-polled `SleepFuture` on a
worker thread may be writing it. `TimerState::waker` is therefore
`std::atomic<std::shared_ptr<detail::Waker>>` (C++20 specialisation), making the store in
`poll()` and the load in `timer_cb` race-free without a mutex.

### Timer resolution

libuv timers have **millisecond resolution**. A `sleep_for` duration shorter than 1ms is
rounded up to 1ms by `StartTimer::execute()` via `std::max<int64_t>(0, ms)`. In practice
this is not a meaningful limitation: the round-trip through the executor and I/O thread
already adds latency on the order of microseconds to milliseconds, so sub-millisecond
timer precision is not achievable regardless. Document this limitation in the `sleep_for`
API comment when implementing.

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

## Relationship to Existing Abstractions

| Abstraction | Change |
|---|---|
| `Waker` / `Context` | None |
| `Executor::enqueue()` | None |
| `SleepFuture::poll()` | Replaces `schedule_wakeup()` with `IoService::submit(StartTimer{...})`; adds `~SleepFuture` for cancellation |
| `TimerService` | **Removed** — replaced by `IoService` |
| `set_current_timer_service` / `schedule_wakeup` | **Removed** — replaced by `set_current_io_service` / `current_io_service()` |
| `Runtime` | Replaces `m_timer_service` with `m_io_service`; calls `set_current_io_service` in `block_on` and worker thread startup |
| All combinators, `spawn`, `JoinHandle` | Unchanged |

---

## I/O Primitives (Follow-on Phase)

Once the event loop is wired in, `include/coro/io/` gets async wrappers for libuv
handles. Each wraps the corresponding `uv_handle_t`, stores a `Waker` in the libuv
callback data, and calls `wake()` when the operation completes. From the user's
perspective these are ordinary `Future`s and `Stream`s:

```cpp
coro::Coro<void> run() {
    auto listener = co_await coro::TcpListener::bind("0.0.0.0", 8080);
    while (auto conn = co_await coro::next(listener)) {
        coro::spawn(handle_connection(std::move(*conn))).submit().detach();
    }
}
```

Each I/O operation follows the same pattern as the timer: allocate a handle state struct,
submit an `IoRequest` variant, store the `Waker`, and cancel in the destructor if the
operation has not yet completed.

Planned primitives (see `roadmap.md` for full details):

- **`TcpListener` / `TcpStream`** — accept connections, read/write with backpressure
- **`UdpSocket`** — async send/recv
- **`File`** — async read/write via libuv's thread-pool file I/O
- **DNS resolution** — `resolve(hostname)` returning `Future<IpAddress>`

All live in `include/coro/io/`.

---

## Race Conditions and Implementation Hazards

All known concurrency concerns are listed here so they are not overlooked during Phase 2
and 3. **RESOLVED** items are handled by the design above. **CAUTION** items require care
during implementation.

### RESOLVED — `TimerState::waker` concurrent read/write
Worker thread writes `waker` on re-poll; I/O thread reads it in `timer_cb`. Resolved by
`std::atomic<std::shared_ptr<Waker>>` in `TimerState`.

### RESOLVED — Double-close of `uv_timer_t`
`timer_cb` and `CancelTimer::execute()` race to call `uv_close`. Resolved by
`fired.exchange(true)` — the first caller owns the close, the other is a no-op.

### RESOLVED — `TimerState` freed before `CancelTimer` executes
If `SleepFuture` is destroyed before the I/O thread processes `CancelTimer`, the state
must remain alive. Resolved by `CancelTimer` holding `shared_ptr<TimerState>`.

### RESOLVED — `m_stopping` read without the lock
`io_async_cb` reads `m_stopping` after releasing `m_io_queue_mutex`, while `stop()` writes
it under the lock — a data race. Resolved by making `m_stopping` `std::atomic<bool>`.

### CAUTION — `StartTimer` holds a raw `TimerState*`
`StartTimer::execute()` accesses `state` via a raw pointer. This is safe because either:
(a) `SleepFuture` is still alive and holds the `shared_ptr`, or (b) `SleepFuture` was
destroyed and `CancelTimer` (holding the `shared_ptr`) is behind `StartTimer` in the same
FIFO queue, keeping `TimerState` alive until after `StartTimer::execute()` returns.
This relies on the queue being strictly FIFO and both requests being processed in the same
`process_queue()` drain. Verify this holds during implementation — if the queue is ever
reordered or split, this breaks.

### CAUTION — `TimerState` is not standard-layout; do not use a first-field pointer cast
`TimerState` contains `std::atomic<std::shared_ptr<>>`, which makes it non-standard-layout.
Casting `uv_timer_t*` → `TimerState*` via a first-field pointer assumption is therefore
undefined behaviour. Instead, store a raw `TimerState*` explicitly in `handle->data` inside
`StartTimer::execute()` (immediately after `uv_timer_init`, before `uv_timer_start`), and
recover it in `timer_cb` and `close_cb` via `static_cast<TimerState*>(handle->data)`.

### CAUTION — `close_cb` must release `handle->data` or `TimerState` leaks
`close_cb` is the only place the raw `TimerState*` stored in `handle->data` can be
released back to the `shared_ptr`. If `close_cb` does not do this, the `shared_ptr`
reference count never reaches zero and `TimerState` leaks. The implementation must be:
```cpp
void close_cb(uv_handle_t* handle) {
    delete static_cast<TimerState*>(handle->data);
}
```

### CAUTION — `set_current_io_service` must be called on every worker thread
Every executor worker thread must call `set_current_io_service(&m_io_service)` at startup,
just as they currently call `set_current_timer_service`. If a worker thread is added in the
future without this call, any `SleepFuture` polled on that thread will throw from
`current_io_service()`. Audit all executor worker thread startup paths during implementation.

### CAUTION — Waker lifetime if task is cancelled before the timer fires
`timer_cb` loads `state->waker` and calls `waker->wake()`. `Waker` is `shared_ptr`-managed
so the reference count keeps it alive as long as `TimerState` holds it. The risk is if the
executor destroys a task's `Waker` before the timer fires — e.g. a cancelled task that has
been fully drained before the I/O thread processes its pending timer. Verify that the
cancellation path (PollDropped drain) waits for the `CancelTimer` to be acknowledged before
releasing the task's waker, or that `TimerState::waker` holding a `shared_ptr` is
sufficient to keep it alive until `timer_cb` runs.

### CAUTION — `uv_async_send` after `uv_close(&m_async)`
Once `io_async_cb` calls `uv_close(&m_async)` during shutdown, any subsequent
`uv_async_send(&m_async)` is UB. This is prevented by the shutdown ordering (executor
joins before `stop()` is called), but any future code path that calls `submit()` outside
of an executor worker thread must be audited against this constraint.

### CAUTION — Stray in-flight handles at `uv_loop_close` time
If any `uv_timer_t` handles are still open when `uv_loop_close` is called (e.g. a
`CancelTimer` arrived after the final `process_queue()` drain), `uv_loop_close` returns
`UV_EBUSY`. The design handles this by re-running `uv_run`. Ensure the re-run loop
terminates: it should, because all remaining handles will close in that iteration. If
somehow a close callback opens a new handle, the loop will never exit. Do not open new
handles from close callbacks during shutdown.

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
