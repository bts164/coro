# libuv Integration

## Overview

The library uses libuv as its I/O reactor. A dedicated `SingleThreadedUvExecutor` — itself
a full `Executor` implementation — owns a `uv_loop_t` and runs it on a dedicated thread.
Coroutines that need to make libuv calls are dispatched onto that thread via
`with_context(exec, coro)`; other executors' worker threads communicate with it only
through the normal thread-safe `enqueue()` injection path, exactly like any other
cross-executor task handoff.

`SingleThreadedUvExecutor` backs `sleep_for()` / `SleepFuture` and every async I/O
primitive in `include/coro/io/`: `TcpStream`, `TcpListener`, `UdpSocket`, `WsStream`,
`WsListener`, `File`, and `PollStream`. `Waker`, `Context`, and the `Executor` interface
are unchanged — leaf futures call `ctx.getWaker()`, store it, and the libuv callback
fires it when the event arrives.

---

## The Core Problem: libuv Is Not Thread-Safe

Nearly all libuv API calls (`uv_timer_start`, `uv_read_start`, `uv_tcp_connect`, etc.)
**must be made from the thread that owns the event loop**. Tasks running on other
executors (e.g. a work-stealing pool) cannot call these APIs directly.

The solution is a **dedicated uv thread**, owned by `SingleThreadedUvExecutor`. Any
coroutine that needs to call libuv APIs is scheduled onto that executor via
`with_context()`; once running there, it is guaranteed to be on the loop's owning thread
for its entire lifetime, so calling libuv directly inside it is safe. When libuv fires a
callback, the callback runs on the uv thread and calls `waker->wake()`, which routes the
waiting task back into whichever executor scheduled it via the existing thread-safe
`enqueue()` path.

---

## The Mechanism: `uv_async_t` as a Cross-Thread Doorbell

`uv_async_t` is the **only** truly thread-safe libuv primitive. Calling
`uv_async_send(&async)` from any thread wakes the loop and schedules a callback to fire
on the loop thread. Critically, multiple sends before the callback fires are
**coalesced** — the callback fires at least once but not necessarily once per send. The
callback must therefore drain an entire queue rather than assuming one send = one wakeup.

`SingleThreadedUvExecutor` uses this doorbell for its own coroutine scheduling, not just
for I/O: `enqueue()` called from a remote thread pushes onto `m_incoming_wakes` and calls
`uv_async_send()`; `enqueue()` called from the uv thread itself pushes directly onto the
local `m_ready` queue. Either way, the uv thread's own loop iteration is what actually
drains and polls tasks — see below.

### The uv thread's loop

```
loop:
  drain_incoming_wakes()      // moves m_incoming_wakes → m_ready (remote enqueue() calls)
  drain_ready_tasks()         // polls every task currently in m_ready
  uv_run(loop, UV_RUN_ONCE)   // processes one round of libuv I/O events; blocks when idle
```

`uv_run(UV_RUN_ONCE)` blocking is what lets the OS park the uv thread at the kernel level
(epoll/kqueue) when there's nothing to do — a remote `enqueue()`'s `uv_async_send()` is
what wakes it back up.

### Example: a libuv callback waking a coroutine

```
Some executor's worker thread          uv thread (owns uv_loop, runs SingleThreadedUvExecutor)
──────────────────────────────         ─────────────────────────────────────────────────────
with_context(uv_exec, coro)             coro begins running on the uv thread:
  → spawns coro onto uv_exec              uv_timer_init(loop, &handle)
                                           handle.data = <state pointer>
                                           uv_timer_start(&handle, timer_cb, delay_ms, 0)
                                           co_await wait(result)   // suspends; stores Waker

                                         ... uv_run(UV_RUN_ONCE) blocks until deadline ...

                                         timer_cb(uv_timer_t* h):
                                           result->complete(...)       // stores value
                                                                       // wakes stored Waker
                                                                       ──► executor->enqueue(task)
                                         (task re-polled, sees the value, proceeds)
```

---

## `SingleThreadedUvExecutor`

The uv loop lives inside `SingleThreadedUvExecutor` (`include/coro/runtime/single_threaded_uv_executor.h`),
which implements the `Executor` interface directly — there is no separate
I/O-service-versus-executor split. `Runtime` owns exactly one `m_uv_executor` member;
there is no second I/O object whose destruction must be sequenced relative to a
work-stealing executor.

```cpp
// include/coro/runtime/single_threaded_uv_executor.h
class SingleThreadedUvExecutor : public Executor {
public:
    SingleThreadedUvExecutor(Runtime* runtime = nullptr);
    ~SingleThreadedUvExecutor() override;

    void schedule(std::shared_ptr<detail::TaskBase> task) override;
    void enqueue(std::shared_ptr<detail::TaskBase> task) override;
    void wait_for_completion(detail::TaskStateBase& state) override;

    void stop();

    lws_context* lws_ctx();               // libwebsockets context, owned here
    uv_loop_t*   loop() noexcept;          // uv thread only

private:
    static void io_async_cb(uv_async_t* handle);
    void io_thread_loop();
    void drain_incoming_wakes();
    void drain_ready_tasks();

    uv_loop_t    m_uv_loop;
    lws_context* m_lws_ctx = nullptr;
    uv_async_t   m_async;

    std::queue<std::shared_ptr<detail::TaskBase>> m_ready;          // uv thread only
    std::deque<std::shared_ptr<detail::TaskBase>> m_incoming_wakes; // remote injection
    std::mutex                                    m_remote_mutex;

    std::thread::id   m_uv_thread_id;
    std::thread       m_uv_thread;
    std::atomic<bool> m_stopping{false};

    std::mutex                                            m_owned_mutex;
    std::unordered_set<std::shared_ptr<detail::TaskBase>> m_owned_tasks;
};

/// Sets the thread-local current SingleThreadedUvExecutor. Called by Runtime.
void set_current_uv_executor(SingleThreadedUvExecutor* exec);

/// Returns the thread-local SingleThreadedUvExecutor, or throws if called
/// outside a Runtime context.
SingleThreadedUvExecutor& current_uv_executor();
```

Note that `SingleThreadedUvExecutor` also owns the `lws_context*` used by the WebSocket
primitives (`WsStream`, `WsListener`) — libwebsockets is configured with
`foreign_loops` pointing at this same `uv_loop_t`, so both libuv and lws callbacks fire
on the same uv thread. See `doc/design/websocket_stream.md`.

---

## `with_context` — Running a Coroutine on the uv Thread

`include/coro/task/spawn_on.h` provides the two functions that route work onto the uv
executor (or any executor):

```cpp
template<Future F>
[[nodiscard]] JoinHandle<typename F::OutputType> spawn_on(Executor& exec, F future);

template<Future F>
[[nodiscard]] JoinHandle<typename F::OutputType> with_context(Executor& exec, F future);
```

`with_context(exec, future)` schedules `future` onto `exec` and returns a `JoinHandle`;
`co_await`-ing it suspends the caller until the child completes and resumes the caller on
whatever executor it was already running on — no explicit "switch back" step is needed.
This is the *only* way I/O primitives call into libuv: every libuv API call in this
codebase happens inside a coroutine passed to `with_context(*m_uv_exec, ...)`.

As covered in CLAUDE.md's conventions, the lambda coroutine passed to `with_context`
must never capture — all data must be passed as explicit parameters, since the closure
is destroyed before the first `co_await` runs.

---

## `UvCallbackResult` / `UvFuture` — Bridging libuv Callbacks to Coroutines

`include/coro/runtime/uv_future.h` replaces the old per-operation `IoRequest` subclass
pattern with a single, reusable, generic bridge. There's no polymorphic command object at
all — just a small piece of shared state allocated on the coroutine's own frame.

```cpp
template<typename... Args>
struct UvCallbackResult {
    std::mutex                         mutex;
    std::shared_ptr<detail::Waker>     waker;   // GUARDED BY mutex
    std::optional<std::tuple<Args...>> value;   // GUARDED BY mutex; set once by complete()

    void complete(Args... args);   // called from the uv callback; thread-safe
};

template<typename... Args>
class UvFuture {
public:
    using OutputType = std::tuple<Args...>;

    explicit UvFuture(UvCallbackResult<Args...>& result);
    UvFuture(UvCallbackResult<Args...>& result, std::function<void()> cancel_fn);

    PollResult<OutputType> poll(detail::Context& ctx);
};

template<typename... Args>
UvFuture<Args...> wait(UvCallbackResult<Args...>& result);
```

Usage pattern, taken directly from `TcpStream::write()` (`src/io/tcp_stream.cpp`):

```cpp
co_await with_context(*m_uv_exec,
    [](Handle* h, std::vector<std::byte> buf) -> Coro<int> {
        UvCallbackResult<int> result;
        uv_buf_t uv_buf = uv_buf_init(reinterpret_cast<char*>(buf.data()), buf.size());
        uv_write_t req;
        req.data = &result;
        uv_write(&req, &h->tcp, &uv_buf, 1, [](uv_write_t* r, int status) {
            static_cast<UvCallbackResult<int>*>(r->data)->complete(status);
        });
        auto [status] = co_await wait(result);
        co_return status;
    }(m_handle.get(), std::move(buf))
);
```

`UvCallbackResult` is allocated on the coroutine's own stack frame; `co_await wait(result)`
keeps the coroutine (and therefore the frame) suspended until `complete()` fires, so no
heap allocation or shared-ownership dance is needed for the common one-shot case. The
optional cancel-function constructor is used when the future may be destroyed before the
callback fires (e.g. `TcpStream::read()`'s per-call `uv_read_start`/`uv_read_stop` pair) —
the cancel function is invoked from `UvFuture`'s destructor.

This single generic type replaces what used to require a bespoke `IoRequest` subclass
(`StartRequest`, `TcpConnectRequest`, `UdpSendRequest`, etc.) per operation.

---

## `SleepFuture` Design

`SleepFuture` (`include/coro/sync/sleep.h`, non-Pico branch) is the simplest example of
the pattern and a good template for any new primitive:

```cpp
class SleepFuture {
public:
    using OutputType = void;

    explicit SleepFuture(std::chrono::nanoseconds duration);
    ~SleepFuture();   // cancels the timer via with_context if not yet fired

    PollResult<void> poll(detail::Context& ctx);

private:
    struct State : std::enable_shared_from_this<State> {
        uv_timer_t                                  handle;
        std::atomic<std::shared_ptr<detail::Waker>> waker;  // RACE: written by poll(), read by timer_cb
        std::atomic<bool>                           fired{false};
    };

    static void timer_cb(uv_timer_t* handle);
    static void close_cb(uv_handle_t* handle);

    std::chrono::time_point<std::chrono::steady_clock,
                            std::chrono::milliseconds> m_deadline;
    std::shared_ptr<State>    m_state;
    SingleThreadedUvExecutor* m_uv_exec = nullptr;  // cached on first poll()
};
```

On the first `poll()`, `SleepFuture` caches `&current_uv_executor()` (so its destructor
can cancel the timer even if the thread-local has already been cleared by the time it
runs), allocates a `State`, and calls `with_context(*m_uv_exec, ...)` to register a
one-shot `uv_timer_t` on the uv thread. `timer_cb` fires the stored waker and closes the
handle. A `fired.exchange(true)` guards against double-close between `timer_cb` and the
destructor's cancellation path racing each other.

### Timer resolution

libuv timers have **millisecond resolution**, and `loop->time` (frozen at the start of
each loop iteration) can lag `steady_clock` by up to 1ms, so a timer can fire marginally
before its logical deadline. `poll()` detects this (`state->fired` true but
`steady_clock::now() < m_deadline`), discards the state, and reschedules for the small
remainder — this reschedule is guaranteed not to fire early a second time, since by then
`loop->time` will have caught up past the original deadline.

---

## Shutdown

```
SingleThreadedUvExecutor::stop():
  m_stopping = true
  uv_async_send(&m_async)     // wake the uv thread one last time

io_async_cb (uv thread):
  drain_incoming_wakes(); drain_ready_tasks()   // finish any last work
  if (m_stopping):
    uv_close(&m_async, nullptr)   // last ref-counted handle closing lets uv_run() return

// uv_run() returns → io_thread_loop() exits → m_uv_thread.join() completes in the destructor.
```

If any handles (timers, sockets, etc.) are not yet closed when the loop is asked to stop
(e.g. in-flight cancellations that arrive concurrently with `stop()`), `uv_loop_close`
returns `UV_EBUSY`; the implementation runs `uv_run` again to flush the remaining close
callbacks before retrying `uv_loop_close`.

Because `SingleThreadedUvExecutor` **is** the executor (not a second object alongside
one), there is no cross-object declaration-order invariant to maintain in `Runtime` the
way an older design (with a separate I/O-service object) would have needed — `Runtime`
owns a single `m_uv_executor` member and its destructor handles shutdown directly.

---

## Why a Dedicated uv Thread Instead of `UV_RUN_NOWAIT`

An alternative design drives the event loop inline — calling
`uv_run(loop, UV_RUN_NOWAIT)` from a worker thread between task polls. This avoids a
separate thread but has several drawbacks:

- `uv_run` must still be called by the loop's owner thread. With work-stealing, any
  worker could end up calling it, which requires strict ownership tracking.
- `UV_RUN_NOWAIT` is inherently polling — it only processes callbacks that are already
  ready. Timers that expire while no tasks are running still require the loop to tick,
  which needs a dedicated caller.
- `UV_RUN_DEFAULT`/`UV_RUN_ONCE` with a dedicated thread lets the OS park that thread at
  the kernel level between events (epoll/kqueue wait). `UV_RUN_NOWAIT` misses this
  entirely.

A dedicated uv thread is simpler, matches how Tokio structures its I/O driver, and
cleanly separates scheduling concerns (handled identically to any other `Executor`) from
I/O concerns (handled by libuv calls made only from coroutines run via `with_context`).

---

## Relationship to Other Abstractions

| Abstraction | Notes |
|---|---|
| `Waker` / `Context` | Unchanged |
| `Executor::enqueue()` | Unchanged — I/O callbacks wake tasks via the normal injection path |
| `SleepFuture` | Runs its timer setup/teardown via `with_context(*m_uv_exec, ...)` |
| `Runtime` | Owns `m_uv_executor` (a `SingleThreadedUvExecutor`); calls `set_current_uv_executor` at startup |
| All combinators, `spawn`, `JoinHandle` | Unchanged |

---

## I/O Primitives

`include/coro/io/` contains async wrappers built on `SingleThreadedUvExecutor`. Each
follows the same pattern: run a coroutine on the uv executor via `with_context`, arm a
libuv operation, `co_await wait(result)` on a stack-allocated `UvCallbackResult`, and
cancel any still-pending operation in the destructor via the same `with_context`
mechanism.

- **`TcpStream`** — async TCP connection: `connect()`, `read()`, `write()`.
- **`TcpListener`** — async TCP accept loop: `bind()`, `accept()`.
- **`UdpSocket`** — async connectionless datagrams: `bind()`, `send_to()`, `recv_from()`.
  See `doc/design/udp_socket.md` for the full design (including the lwIP/Pico backend).
- **`WsStream`** — async WebSocket client: `connect()`, `send()`, `receive()`.
- **`WsListener`** — async WebSocket server: `bind()`, `accept()`.
- **`File`** — async filesystem I/O via libuv's thread-pool file operations: `open()`,
  `read()`, `write()`, `close()`.
- **`PollStream`** — generic `uv_poll_t`-based readiness notification for raw file
  descriptors that don't fit the above (see `doc/design/poll_streams.md`).

DNS resolution is not yet a standalone primitive; `TcpStream::connect()` resolves
hostnames internally via libuv's `uv_getaddrinfo`.

---

## Race Conditions

Known concurrency concerns and how they are resolved:

**`State::waker` concurrent read/write** (`SleepFuture` and similar) — the executor
thread writes `waker` on re-poll; the uv thread reads it in the libuv callback. Resolved
by `std::atomic<std::shared_ptr<Waker>>` (or, in `UvCallbackResult`, a plain `mutex`
guarding both `waker` and `value`).

**Double-close of a libuv handle** — both the success callback (e.g. `timer_cb`) and a
cancellation path (destructor via `with_context`) may attempt to close the same handle.
Resolved by a `fired.exchange(true)` (or equivalent) claim: the first caller owns the
close, the other is a safe no-op.

**`UvCallbackResult` state outliving the frame** — since `UvCallbackResult` is stack
allocated in a coroutine passed to `with_context`, the coroutine must not return until
the libuv callback either fires or is guaranteed never to fire (cancelled). Operations
that can outlive a single `co_await` (e.g. `TcpStream::read()`'s persistent
`uv_read_start`) use the `UvFuture` cancel-function constructor to guarantee the
callback is disarmed (`uv_read_stop`) before the frame is torn down.

**`m_stopping` read/write race** — `io_async_cb` reads `m_stopping` while `stop()` writes
it from another thread. Resolved by `std::atomic<bool> m_stopping`.

**Stray in-flight handles at `uv_loop_close` time** — if cancellations arrive after the
final drain, `uv_loop_close` returns `UV_EBUSY`. Handled by re-running `uv_run` once more
to flush remaining close callbacks.

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
