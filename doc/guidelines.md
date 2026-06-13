# Library Guidelines

Rules for writing correct, safe, and idiomatic code with this library. Modelled on the
[C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) format:
each rule has a short title, a rationale, and concrete examples.

---

## Table of Contents

- [Coroutine Scope](#coroutine-scope)
    - CS.1 — Do not drop a `JoinHandle` while the task holds references to local data
    - CS.2 — Use `co_invoke` to place the task handle in a nested scope when referencing local data
    - CS.3 — Never invoke a capturing lambda coroutine directly; use `co_invoke`
    - CS.4 — Be careful when spawning member function coroutines; `this` is implicitly captured
    - CS.5 — Detached tasks must own all their data
- [Spawning and Ownership](#spawning-and-ownership)
- [Cancellation Safety](#cancellation-safety)
    - CA.1 — Use RAII for all resources owned by a coroutine frame
    - CA.2 — Do not assume cancelled futures stop immediately; they drain before completing
    - CA.3 — Check for cancellation in long-running loops
    - CA.4 — Do not put blocking destructors in futures that may be cancelled as a losing branch
- [Blocking Work](#blocking-work)
    - BL.5 — Do not call `MpscReceiver::blocking_recv()` from a coroutine
- [Channels](#channels)
    - CH.5 — Do not await `Event::wait()` from more than one coroutine at a time
- [Select and Timeout](#select-and-timeout)
- [Silent Cancellation](#silent-cancellation)
- [Error Handling](#error-handling)
- [ISR Safety](#isr-safety-mcu-platforms--coro_pico-only)
    - IS.1 — Only call `signal_from_isr()` or `send_from_isr()` from an interrupt handler
    - IS.2 — Keep interrupt handlers minimal: write a flag or value and return immediately
    - IS.3 — Declare `IsrEvent` and `IsrChannel<T>` at static or class scope; never as coroutine locals

---

## Coroutine Scope

### CS.1 — Do not drop a `JoinHandle` while the task holds references to local data

**Reason:** If a coroutine spawns a child task that holds a reference to a local variable
and that local is destroyed while the child is still running, the child reads dangling
memory. Unlike Rust, C++ has no borrow checker to catch this at compile time.

The coroutine scope mechanism provides a runtime safety net: the parent frame is not freed
until all non-detached children have drained. However, there is a window at `co_return` and
at thrown exceptions where locals are destroyed before the drain wait begins — which is the
source of the pitfalls below.

```cpp
// BAD — local_data is destroyed at co_return before the drain begins.
Coro<void> bad() {
    int local_data = 42;
    auto h = spawn(worker(&local_data));
    co_return;  // local_data destroyed here; drain happens after, too late
}

// BAD — an exception between the spawn and the co_await destroys local_data
// before the drain begins.
Coro<void> fragile() {
    int local_data = 42;
    auto h = spawn(worker(&local_data));
    co_await might_throw();  // if this throws, local_data is gone before drain
    // h dropped here → drain begins, but local_data already destroyed
}

// BAD — looks safe but is not: any exception thrown before co_await js.drain()
// bypasses the drain and destroys local_data while children are still running.
Coro<void> deceptive() {
    int local_data = 42;
    JoinSet<void> js;
    js.spawn(worker(&local_data));
    co_await js.drain();  // NOT reached if anything above this line throws
}

// GOOD — use co_invoke to push spawning into an inner coroutine. The outer
// frame (and local_data) stays suspended at co_await co_invoke() for the
// entire inner lifetime, including through exceptions in the inner body.
Coro<void> safe(int local_data) {
    co_await co_invoke([](int& data) -> Coro<void> {
        JoinSet<void> js;
        js.spawn(worker(&data));
        co_await js.drain();
    }(local_data));
}
```

### CS.2 — Use `co_invoke` to place the task handle in a nested scope when referencing local data

**Reason:** The fix for the pitfalls in CS.1 is a scope relationship: the referenced
variable lives in an *outer* frame while the `JoinHandle` (or `JoinSet`) lives in an
*inner* `co_invoke` scope. The outer frame stays suspended at `co_await co_invoke(...)`
for the entire inner lifetime — including through exceptions — so the referenced data is
always alive as long as any child spawned inside runs.

What makes the pattern safe is not which handle type is used, but that the handle lives
inside the `co_invoke` scope while the referenced variable lives outside it.

```cpp
// GOOD — single task; JoinHandle in the inner scope, data in the outer frame
Coro<void> process_one(Item& item) {
    co_await co_invoke([](Item& item) -> Coro<void> {
        auto h = spawn(process_item(item));
        co_await h;
    }(item));
}

// GOOD — multiple tasks; JoinSet in the inner scope, items in the outer frame
Coro<void> process_many(std::vector<Item>& items) {
    co_await co_invoke([](std::vector<Item>& items) -> Coro<void> {
        JoinSet<void> js;
        for (auto& item : items)
            js.spawn(process_item(item));
        co_await js.drain();
    }(items));
}
```

### CS.3 — Never invoke a capturing lambda coroutine directly; use `co_invoke`

**Reason:** Capturing lambda coroutines are a common source of obscure use-after-free
errors. The C++ Core Guidelines go so far as to recommend in their very first coroutine
rule that capturing lambda coroutines should never be used at all. We can be a little less
strict because `co_invoke` exists specifically to handle this problem safely.

The problem is that the C++ compiler transforms a capturing lambda into an anonymous struct
whose data members are the captures, and whose `operator()` is the function body. When
`operator()` is a coroutine, the coroutine frame captures `this` — a pointer to the lambda
struct — to access those members across suspension points. If the lambda object is a
temporary (e.g. immediately invoked with `()`), it is destroyed as soon as `operator()`
returns the `Coro<T>` handle. The coroutine is left holding a dangling `this` pointer.
Every access to a captured variable after the first suspension point is a use-after-free.

This is easy to miss because the lambda syntax hides what the compiler actually generates.
To understand it better, consider the following seemingly innocent-looking example:

```cpp
Coro<void> coro = [data = my_data]() -> Coro<void> {
    co_await something();
    use(data);
}();
co_await coro;
```

But the compiler internally transforms it to roughly this, which makes the lifetime problem much more obvious:

```cpp
struct __lambda {
    Data data;
    Coro<void> operator()() {
        // The coroutine frame captures 'this' (pointer to the __lambda object)
        // so it can access 'data' across suspension points. But by the time
        // the coroutine resumes during co_await, the __lambda has been destroyed.
        co_await something();
        use(this->data);  // use-after-free: 'this' points to destroyed memory
    }
};

Coro<void> coro = __lambda{my_data}();  // temporary __lambda constructed, operator() called,
                                        // temporary __lambda immediately destroyed
                                        // saved Coro<void> references 'this' (the __lambda)
co_await coro;                          // coroutine resumes — accesses dangling 'this'
```

`co_invoke` exists specifically to solve this. It returns a `CoInvokeFuture` — a RAII
handle that owns both the coroutine and the lambda object on the heap, tying their
lifetimes together so they are always destroyed together. The heap allocation also pins
the lambda's address: even if the `CoInvokeFuture` itself is moved, the lambda it points
to does not move. Both properties together guarantee that `this` is always valid.

```cpp
// GOOD — co_invoke stores the lambda inside the CoInvokeFuture; lifetimes are tied together
co_await co_invoke([data = my_data]() -> Coro<void> {
    co_await something();
    use(data);  // 'this' points into the co_invoke frame — valid for the coroutine's lifetime
});
```

A named variable that outlives the coroutine also works, but is fragile:

```cpp
// OK but fragile — lambda must remain alive and unmoved for the entire duration of coro
auto lambda = [data = my_data]() -> Coro<void> {
    co_await something();
    use(data);
};
auto coro = lambda();
co_await coro;  // safe only because lambda is still alive and has not been moved
```

This breaks the moment `coro` is stored and awaited somewhere else, or if `lambda` is
ever moved. Prefer `co_invoke` in all cases.

**Rule of thumb:** if a lambda is a coroutine and captures anything, always wrap it in
`co_invoke` rather than invoking it directly.

### CS.4 — Be careful when spawning member function coroutines; `this` is implicitly captured

**Reason:** A non-static member function coroutine implicitly captures `this` into its
frame for the same reason a capturing lambda does — the coroutine body is lowered to a
method on the containing object, and accessing any member variable across a suspension
point reads through `this`. If the object is destroyed while the coroutine is still
running, every subsequent member access is a use-after-free.

**`co_await` is safe; `spawn` requires care.** When you `co_await` a member coroutine
directly, structured cancellation guarantees the coroutine finishes before the enclosing
scope resumes — and the enclosing scope owns the object — so `this` is always valid:

```cpp
struct Processor {
    int value = 42;

    Coro<int> compute() {
        co_await something();
        co_return value;  // safe — 'this' is alive; co_await blocks the caller's scope
    }
};

Coro<void> run() {
    Processor p;
    int result = co_await p.compute();  // safe — compute() finishes before run() can resume and destruct p
};
```

Spawning is different. `spawn()` schedules the task independently; `p` can be destroyed
while `compute()` is still running:

```cpp
Coro<void> run() {
    Processor p;
    auto h = coro::spawn(p.compute());  // compute() holds implicit 'this' → &p

    co_return;
    // ① h destroyed (reverse declaration order) — compute() receives cancellation signal
    // ② p destroyed — p.value is gone
    // ③ scope drain resumes compute() — accesses p.value — use-after-free
}

Coro<void> run_detached() {
    Processor p;
    coro::spawn(p.compute()).detach();  // compute() fully outlives p
    co_return;                          // p destroyed here; compute() still running
};
```

Two safe patterns:

**Option 1 — `co_invoke` scope.** Wrap the spawn in a `co_invoke` scope that the object's
owner awaits, guaranteeing the object outlives the scope:

```cpp
Coro<void> run() {
    Worker w; // outlives the co_invoke — run() suspends here until the spawned task drains
    co_await co_invoke([&]() -> Coro<void> {  // [&] safe here — co_invoke owns the lambda
        auto h = spawn(w.task());              // w is alive for the entire co_invoke scope
        co_await h;
    });
}
```

**Option 2 — static method with explicit `shared_ptr` self.** Convert the member
coroutine to a static function that takes ownership of the object via `shared_ptr`. The
lifetime of `self` is tied to the coroutine frame, so the object cannot be destroyed while
the task is running, regardless of when the original owner goes away:

```cpp
struct Worker : std::enable_shared_from_this<Worker> {
    int data = 0;

    static Coro<void> task(std::shared_ptr<Worker> self) {
        co_await long_running();
        use(self->data);  // safe — self keeps Worker alive
    }

    Coro<void> start() {
        spawn(task(shared_from_this())).detach();  // safe — task owns a ref
        co_return;
    }
};
```

The static-method approach gives up the implicit `this` syntax in exchange for an
explicit, auditable lifetime guarantee. Prefer it whenever a member coroutine may be
spawned without a containing scope that provably outlives it.

### CS.5 — Detached tasks must own all their data

**Reason:** `JoinHandle::detach()` explicitly opts the task out of the coroutine scope
mechanism. The library makes no lifetime guarantee for detached tasks — they may be
scheduled and run after every local variable in the spawning coroutine has been destroyed.
This is the same constraint Rust enforces with its `'static` bound on `tokio::spawn`,
except here it cannot be checked at compile time.

```cpp
// BAD — detached task captures a reference to a local
Coro<void> bad() {
    std::string msg = "hello";
    spawn(send_message(msg)).detach();  // msg may be gone before task runs
    co_return;
}

// GOOD — move the data into the task
Coro<void> good() {
    std::string msg = "hello";
    spawn(send_message(std::move(msg))).detach();
    co_return;
}
```

---

## Spawning and Ownership

### SP.1 — Prefer `JoinSet` over raw `JoinHandle`s for multiple concurrent tasks

**Reason:** A `std::vector<JoinHandle<T>>` requires the caller to know in advance how many
tasks there are, await them in a fixed order, and manually manage errors and cancellation.
`JoinSet` handles all of this: it stores handles internally, delivers results in completion
order (not spawn order), provides a single `drain()` call for cleanup, and integrates with
`select` via the `Stream` interface. It is also the correct structured-concurrency scope
boundary — dropping a `JoinSet` cancels all pending children cleanly.

```cpp
// OK but verbose — awaits in spawn order, not completion order
std::vector<JoinHandle<int>> handles;
for (auto& item : items)
    handles.push_back(spawn(compute(item)));
for (auto& h : handles)
    results.push_back(co_await h);

// BETTER — completion order, automatic handle management
JoinSet<int> js;
for (auto& item : items)
    js.spawn(compute(item));
while (auto r = co_await next(js))
    results.push_back(*r);
```

### SP.2 — Use `build_task().name("...").spawn(f)` for named tasks

**Reason:** `spawn(f)` immediately schedules the task and returns its `JoinHandle`.
When you need to attach a name (visible in diagnostics) or set a stream buffer size,
use the `build_task()` builder instead. Forgetting to call `.spawn()` at the end of the
builder chain discards the task — `[[nodiscard]]` on `SpawnBuilder` catches the fully-
discarded case.

```cpp
// Direct spawn — most common path
auto h = spawn(compute());
co_await some_work();
co_await h;

// Named task via builder
auto h2 = build_task().name("serve-connection").spawn(serve_connection(conn));

// Named stream with custom buffer
auto sh = build_task().name("packet-reader").buffer(128).spawn(read_packets(sock));
```

---

## Cancellation Safety

### CA.1 — Use RAII for all resources owned by a coroutine frame

**Reason:** Cancellation does not propagate via exceptions. When a coroutine is cancelled
while suspended at a `co_await` point it is simply never resumed again — the only code
that still runs is the destructors of objects on the coroutine frame, in reverse
declaration order. A `try`/`catch` or `try`/`finally`-style guard cannot protect against
this: if the coroutine is suspended inside the `try` block it will never reach the
corresponding `catch` or cleanup code. The destructors are all that execute.

This makes RAII more important in coroutines than in ordinary synchronous functions. In
synchronous code an exception is always catchable at the call site; in a coroutine,
cancellation is a silent exit path that bypasses every piece of non-destructor cleanup.
Any resource that must be released — a file handle, a lock, a connection, heap memory —
must be owned by a RAII handle whose destructor performs the release.

```cpp
// BAD — try/catch looks safe but is bypassed if the coroutine is cancelled
// while suspended. Cancellation does not throw; the coroutine simply stops
// being resumed. The catch block and the delete never execute.
Coro<void> bad() {
    auto* buf = new char[4096];
    try {
        co_await some_io();   // cancelled here — coroutine frame is destroyed,
                              // catch block never runs, buf is leaked
        delete[] buf;
    } catch (...) {
        delete[] buf;         // handles exceptions, but NOT cancellation
        throw;
    }
}

// GOOD — destructor runs regardless of how the coroutine exits,
// including cancellation
Coro<void> good() {
    auto buf = std::make_unique<char[]>(4096);
    co_await some_io();   // cancellation here destroys buf via ~unique_ptr
}
```

The same applies to mutexes, file descriptors, and any other resource with an explicit
release step. `std::lock_guard`, `std::unique_lock`, RAII file wrappers, and similar
handles all guarantee cleanup through their destructor — `try`/`catch` blocks do not.

### CA.2 — Do not assume cancelled futures stop immediately; they drain before completing

**Reason:** Cancellation is cooperative and asynchronous, not instantaneous. When a future
is cancelled, it must first drain its currently-awaited sub-future (waiting for it to return
`PollDropped` if it is itself `Cancellable`, or to complete normally if it is not), then
`handle.destroy()` is called to free the frame (running local destructors in LIFO order),
and finally any child tasks registered with the coroutine scope must drain before
`PollDropped` is returned. This differs fundamentally from Rust, where dropping a future
instantly frees its memory. Code that assumes cancellation is instant may observe resources
living longer than expected, or may try to reuse a resource while the drain is still in
progress.

```cpp
// The timeout future holds the winning result internally until
// the wrapped future has fully drained — then delivers it.
auto result = co_await timeout(5s, long_running_task());
// By the time this line is reached, long_running_task's frame has been
// fully destroyed and all its children have drained.
```

### CA.3 — Check for cancellation in long-running loops

**Reason:** The cancellation flag is checked only at `co_await` points. A coroutine that
runs a tight loop without ever suspending will ignore cancellation for the entire duration
of that loop, defeating the purpose of the scope drain mechanism and causing tasks to
linger long after they are supposed to have stopped. Periodic `co_await yield_now()` calls
give the executor a chance to observe the flag and drive the task to `PollDropped`.

```cpp
// BAD — cancellation not observed until the loop finishes
Coro<void> bad(std::vector<Item> items) {
    for (auto& item : items)
        process(item);  // no suspension; cancellation ignored
    co_return;
}

// GOOD — yield periodically so cancellation is observed
Coro<void> good(std::vector<Item> items) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        process(items[i]);
        if (i % 64 == 0)
            co_await yield_now();  // allows cancellation to be observed
    }
}
```

### CA.4 — Do not put blocking destructors in futures that may be cancelled as a losing branch

**Reason:** When a branch is cancelled (losing a `select` or expiring a `timeout`), its
drain runs on whichever executor worker thread happens to poll it next. A destructor that
blocks that thread — joining a `std::thread`, waiting on a condition variable, flushing a
synchronous buffer — stalls all other tasks scheduled on that worker for the duration.
Blocking destructors belong in `spawn_blocking` callables, not in coroutine locals.

```cpp
// BAD — destructor blocks
struct BlockingResource {
    ~BlockingResource() {
        completion_thread.join();  // blocks an executor thread during drain
    }
    std::thread completion_thread;
};

Coro<void> bad_branch() {
    BlockingResource r;  // destructor will block the executor during drain
    co_await long_work();
}

// GOOD — use spawn_blocking to do the join off the executor thread, or
// restructure so the resource is cleaned up before any select/timeout.
```

---

## Blocking Work

### BL.1 — Never block on an executor worker thread

**Reason:** Executor worker threads are shared across all running tasks. Blocking one —
even briefly — starves every other task waiting to run on that thread. In a single-threaded
runtime this halts the entire program. In a work-stealing runtime it reduces parallelism
and may cause tasks waiting on I/O to miss their wakeups. `spawn_blocking` exists
specifically to move blocking work onto a separate pool that is allowed to block.

```cpp
// BAD — blocks the executor worker
Coro<void> bad() {
    auto data = read_file_sync("/etc/hosts");  // blocks the worker thread
    co_return;
}

// GOOD — run the blocking call on the blocking pool
Coro<void> good() {
    auto data = co_await spawn_blocking([]() {
        return read_file_sync("/etc/hosts");
    });
}
```

### BL.2 — Never call `blocking_get()` from a coroutine

**Reason:** `BlockingHandle::blocking_get()` synchronously waits for the blocking thread
to finish. Calling it from a coroutine running on an executor worker thread is identical
to any other blocking call on that thread — it stalls the worker and starves other tasks.
`blocking_get()` exists exclusively for use inside other `spawn_blocking` callables, where
the calling thread is already a blocking pool thread and is allowed to block.

```cpp
// BAD — blocks an executor worker
Coro<void> bad() {
    auto handle = spawn_blocking([]() { return compute(); });
    int result = handle.blocking_get();  // blocks the worker thread
    co_return;
}

// GOOD — co_await the handle
Coro<void> good() {
    int result = co_await spawn_blocking([]() { return compute(); });
}

// ALSO GOOD — blocking_get() from within another spawn_blocking callable
int result = co_await spawn_blocking([]() -> int {
    auto inner = spawn_blocking([]() { return sub_compute(); });
    return inner.blocking_get();  // safe — this is a blocking pool thread, not a worker
});
```

### BL.3 — Move data into `spawn_blocking` callables; do not capture by reference

**Reason:** Dropping a `BlockingHandle` detaches the blocking thread rather than waiting
for it. Unlike async `spawn` (which can be cooperatively cancelled), a blocking thread
cannot be interrupted — waiting for it on drop could deadlock. Because the detach is
unconditional, there is no drain guarantee: the thread may still be running when the
spawning coroutine's locals are destroyed. A reference capture into a detached thread is
a use-after-free waiting to happen. This is the C++ analogue of Rust's `'static + Send`
bound on `spawn_blocking` closures.

```cpp
// BAD — reference to local; if handle is dropped, data is a dangling ref
Coro<void> bad() {
    std::string data = load_data();
    auto h = spawn_blocking([&data]() { return process(data); });
    co_return;  // handle dropped → detach → data destroyed → thread reads garbage
}

// GOOD — move data into the callable
Coro<void> good() {
    std::string data = load_data();
    auto h = spawn_blocking([data = std::move(data)]() { return process(data); });
    co_return;
}
```

### BL.5 — Do not call `MpscReceiver::blocking_recv()` from a coroutine

**Reason:** `blocking_recv()` is a synchronous, thread-blocking receive designed for plain
OS threads that cannot `co_await`. Calling it from a coroutine running on an executor
worker thread blocks that worker until an item arrives — starving every other task
scheduled on that thread, identical to any other blocking call (BL.1). Use
`co_await next(rx)` from coroutines; reserve `blocking_recv()` for `std::thread` bodies
or `spawn_blocking` callables.

```cpp
// BAD — blocks the executor worker until an item arrives
Coro<void> bad(MpscReceiver<int> rx) {
    std::optional<int> v = rx.blocking_recv();  // blocks the worker thread
}

// GOOD — suspends the coroutine; executor thread stays free
Coro<void> good(MpscReceiver<int> rx) {
    while (std::optional<int> v = co_await coro::next(rx))
        use(*v);
}

// ALSO GOOD — blocking_recv() from a plain OS thread or spawn_blocking
std::thread([rx = std::move(rx)]() mutable {
    while (std::optional<int> v = rx.blocking_recv())
        use(*v);
}).detach();
```

### BL.4 — Use a nested `Runtime` with `CurrentThreadExecutor` inside `spawn_blocking` to run async code in isolation

**Reason:** Occasionally a third-party library requires its own event loop, or you need
to drive a `Coro<T>` completely independently of the outer runtime (separate I/O state,
separate timer state). Nesting a `Runtime` inside `spawn_blocking` keeps the outer
executor free — the blocking pool thread pays the cost of `block_on`, not a worker —
while giving the inner code full access to `co_await`, timers, and `spawn`.

Prefer `CurrentThreadExecutor` for the nested runtime: it runs its poll loop directly on
the calling (blocking pool) thread with no extra threads of its own. `SingleThreadedExecutor`
and `WorkStealingExecutor` both spawn additional worker threads — wasteful when the
blocking pool thread is already dedicated to this work and allowed to block.

```cpp
Coro<void> run() {
    std::string result = co_await spawn_blocking([]() -> std::string {
        // CurrentThreadExecutor: poll loop runs on this thread; no extra threads spawned.
        Runtime inner(std::in_place_type<CurrentThreadExecutor>);
        return inner.block_on(isolated_work());
    });
}
```

!!! tip "TODO: CurrentThreadExecutor needs desktop defaults"
    `CurrentThreadExecutor` currently requires explicit `ClockFn` and `PollFn` arguments
    (designed for Pico, where `time_us_64` and `cyw43_arch_poll` are injected at
    construction). Two changes are needed before the example above compiles on desktop:

    1. **Default desktop constructor** — add a no-arg (or desktop-defaulted) constructor
       to `CurrentThreadExecutor` that supplies `std::chrono::steady_clock` for the clock
       and a no-op for the poll function.
    2. **Lightweight `Runtime` path** — the desktop `Runtime` constructor currently always
       creates a dedicated libuv thread (`m_uv_executor`) and a blocking pool, because
       `SingleThreadedExecutor` and `WorkStealingExecutor` have no hook for interleaving
       an I/O poll function. `CurrentThreadExecutor` was designed around the opposite
       model: one thread, everything interleaved — the injected `PollFn` is called on
       every loop iteration alongside task polling (on Pico: `cyw43_arch_poll()`; on
       desktop: `uv_run(loop, UV_RUN_NOWAIT)`). When the executor is
       `CurrentThreadExecutor`, the `Runtime` should skip the dedicated libuv thread and
       blocking pool and instead pass a `uv_run(UV_RUN_NOWAIT)` tick as the `PollFn`.

---

## Channels

Channels are the library's embodiment of the Go proverb: *"Do not communicate by sharing
memory; instead, share memory by communicating."* Rather than protecting shared state with
a mutex and having tasks reach in to read or write it, channels let tasks exchange ownership
of values — the sender produces, the receiver consumes, and the channel handles the
synchronization transparently. The rules in this section cover the places where that model
has sharp edges.

### CH.1 — Never hold a `WatchBorrowGuard` across a `co_await` point

**Reason:** `WatchBorrowGuard<T>` holds a shared read lock on the watch channel's value for
its entire lifetime. A shared read lock blocks any `send()` call, which requires the
exclusive write lock. Holding a `WatchBorrowGuard` while suspended means the task is parked —
possibly indefinitely — while the lock is held. Any sender that tries to update the value
will deadlock waiting for the guard to be released. The fix is always to copy the value
out of the guard before suspending.

```cpp
// BAD — WatchBorrowGuard held across co_await
Coro<void> bad(WatchReceiver<Config>& rx) {
    auto guard = rx.borrow();
    co_await do_work(*guard);  // read lock held while suspended — blocks all senders
}

// GOOD — copy the value out before co_await
Coro<void> good(WatchReceiver<Config>& rx) {
    Config config = *rx.borrow();  // copy; guard destroyed at semicolon
    co_await do_work(config);
}

// ALSO GOOD — scope the guard tightly
Coro<void> also_good(WatchReceiver<Config>& rx) {
    {
        auto guard = rx.borrow();
        process_sync(*guard);  // guard released at closing brace
    }
    co_await other_work();
}
```

### CH.2 — Always recover the value from a failed `try_send`

**Reason:** `try_send` is designed around the principle that a failed send should never
silently lose the caller's value. For move-only types (`unique_ptr`, file handles, etc.)
this is critical — if `try_send` consumed the value and then returned an error with no way
to recover it, the caller would have no way to retry or clean up. The `TrySendError<T>`
return type is deliberately designed to give the value back. Ignoring the error case and
letting `r` go out of scope destroys the value with no record of it being undelivered.

```cpp
// BAD — move-only value silently lost on failure
auto r = tx.try_send(std::move(my_unique_ptr));
if (!r) { /* oops — my_unique_ptr is gone and the send failed */ }

// GOOD — recover the value on failure
if (auto r = tx.try_send(std::move(my_unique_ptr)); !r) {
    my_unique_ptr = std::move(r.error().value);  // reclaim it
    // decide: retry later, drop it, or log
}
```

### CH.3 — Use `channel<expected<T, E>>` to propagate application errors

**Reason:** Adding a first-class error-sending mechanism to channels would create two
overlapping error paths at the receiver: transport errors (`ChannelError` from channel
infrastructure) and application errors (sent by the sender). There is no clean way to
distinguish them at the call site, and the API becomes harder to use correctly. The
idiomatic solution — identical to Tokio's — is to make the value type itself a result
type. This gives the receiver two explicit, separately-handled layers with clear semantics:
the outer `expected` is the transport; the inner is the application.

```cpp
// GOOD
auto [tx, rx] = mpsc_channel<std::expected<int, MyError>>(16);

// Sender — success:
co_await tx.send(42);

// Sender — error:
co_await tx.send(std::unexpected(MyError{"failed"}));

// Receiver — two distinct error layers:
auto r = co_await rx.recv();
if (!r) { /* transport error: channel closed */ }
if (!*r) { /* application error: sender sent an error value */ }
int value = **r;
```

### CH.5 — Do not await `Event::wait()` from more than one coroutine at a time

**Reason:** `Event` supports exactly one waiting coroutine. If a second coroutine calls
`wait()` while another is already suspended, the first coroutine's waker is silently
overwritten. When `set()` fires, only the second waiter is woken; the first is parked
indefinitely with no error signal. If multiple tasks need to react to the same signal,
funnel through a single waiting coroutine that then notifies the others via a normal
channel or second-level event.

```cpp
// BAD — two coroutines both await the same event
coro::Event ev;
coro::spawn([]() -> coro::Coro<void> { co_await ev.wait(); /* may never wake */ }());
coro::spawn([]() -> coro::Coro<void> { co_await ev.wait(); /* overwrites first waker */ }());

// GOOD — one waiter fans out via a channel
coro::Coro<void> dispatcher(coro::Event& ev, coro::MpscSender<Signal> tx1,
                             coro::MpscSender<Signal> tx2) {
    co_await ev.wait();
    co_await tx1.send(Signal{});
    co_await tx2.send(Signal{});
}
```

### CH.4 — Do not share a `Sender` or `Receiver` instance between threads

**Reason:** Channel handle objects (`Sender`, `Receiver`, etc.) are standard C++ objects
with no internal synchronization on their own members. Calling methods on the same
instance from two threads concurrently is a data race, producing undefined behaviour. The
underlying channel state (ring buffer, waker lists, ref counts) is always mutex-protected
and safe from any number of threads. The solution is to give each thread its own handle:
for senders, use `clone()`; for receivers, which are single-owner by design, move the
object to the thread that needs it.

```cpp
// BAD — two threads call methods on the same Sender instance
std::thread([&tx]() { tx.try_send(1); }).detach();
tx.try_send(2);  // data race on tx

// GOOD — give each thread its own clone
auto tx2 = tx.clone();
std::thread([tx2 = std::move(tx2)]() mutable { tx2.try_send(1); }).detach();
tx.try_send(2);  // tx and tx2 are independent objects
```

---

## Select and Timeout

### ST.1 — Design `select` branches to drain quickly; the losing branch blocks result delivery

**Reason:** C++ futures cannot be freed without running destructors. When a `select`
branch loses, it cannot simply be dropped — it must be polled to `PollDropped` so its
frame is properly torn down and its children drain. The winning result is held inside
`SelectFuture` and not delivered until this drain completes. For branches with deep task
trees or slow destructors this drain is observable. Designing branches to exit cleanly and
quickly keeps `select` responsive.

```cpp
// The result of select is not available until long_branch has fully drained.
auto result = co_await select(fast_future(), long_branch());
// By here, long_branch's frame is gone and all its children are drained.
```

Design branches to drain quickly. If a branch holds a resource that takes time to release,
consider releasing it before the final suspension point.

### ST.2 — Do not put side-effecting or slow destructors in `select` branches

**Reason:** Destructors in a losing branch run during the async drain on an executor
worker thread — the same thread that needs to be free to run other tasks. A slow
destructor (joining a thread, flushing a buffer, calling a synchronous API) holds up that
worker for as long as the destructor takes to complete. Blocking destructors belong in
`spawn_blocking` callables; resources that need async cleanup should be closed with an
explicit `co_await close()` before the branch's final suspension point.

```cpp
// BAD — slow destructor in a losing branch
Coro<void> slow_branch() {
    SlowDestructorResource r;  // destructor flushes a large buffer synchronously
    co_await work();
}

auto result = co_await select(fast(), slow_branch());
// Drain of slow_branch blocks the executor worker while flushing
```

### ST.3 — `timeout` has the same drain behaviour as `select`; treat it identically

**Reason:** `timeout(dur, f)` is implemented directly as `select(sleep_for(dur), f)`.
Every rule about `select` drain behaviour — latency, slow destructors, resource lifetimes
— applies identically to `timeout`. When the timeout fires, `f` is cancelled and drained
before the `timeout` result is delivered. When `f` completes first, `sleep_for` is
cancelled (fast, since timer cancellation is nearly instant). Treating `timeout` as a
black box that "just cancels" is the most common source of `timeout` surprises.

```cpp
// result is not available until f has fully drained if the timeout fires
auto result = co_await timeout(100ms, f());
```

### ST.4 — Use `coro::ref(f)` to keep a future alive across a losing `select` branch

**Reason:** `select()` takes futures by value and cancels any branch that does not win. If
you want the underlying work to keep running — for example, polling a long-running task
across multiple select rounds while also watching for an external signal — passing the
future directly would cancel it the first time its branch loses.

`coro::ref(f)` wraps a future in a non-owning `FutureRef<F>` that delegates `poll()` to `f`
without taking ownership. The wrapper is discarded when the branch loses; the underlying
future is untouched.

```cpp
// BAD — task is cancelled the first time the signal branch wins
while (true) {
    auto sel = co_await select(spawn(long_work()), signal);
    // task future was consumed and cancelled if signal won
}

// GOOD — task keeps running across multiple rounds
JoinHandle<Result> task = spawn(long_work());
while (true) {
    auto sel = co_await select(coro::ref(task), signal);
    if (std::holds_alternative<SelectBranch<1, void>>(sel)) continue;  // signal fired
    Result r = std::get<SelectBranch<0, Result>>(sel).value;           // task done
    break;
}
```

Two rules to remember when using `coro::ref()`:

1. **Lvalue only.** `coro::ref()` only accepts lvalues. Store the future in a named variable
   first; `coro::ref(spawn(f()))` is a compile error. This guarantees the underlying future
   has a lifetime longer than the wrapper.
2. **Result consumption.** If the `coro::ref(f)` branch wins, the result is moved out of `f`.
   Do not await `f` again afterward — it is logically spent even though it was never moved.

---

## Silent Cancellation

### SC.1 — Never discard a `JoinHandle` without intent

**Reason:** Dropping a `JoinHandle` is not a no-op — it cancels the task and registers
it as a pending child of the enclosing coroutine, delaying that coroutine's next
suspension until the child drains. This can cause unexpected latency and work loss that
is hard to diagnose because there is no error signal. `[[nodiscard]]` catches the
fully-discarded case but not the "assign to a local that immediately goes out of scope"
case. Always be explicit about the intended lifecycle.

```cpp
// BAD — task is immediately cancelled
spawn(important_work());  // warning: [[nodiscard]]

// GOOD options:
auto h = spawn(important_work());        // await later
spawn(fire_and_forget()).detach();       // explicit detach
js.spawn(important_work());             // JoinSet takes ownership
```

### SC.2 — Never discard a `BlockingHandle` without intent

**Reason:** Dropping a `BlockingHandle` unconditionally detaches the blocking thread —
the callable runs to completion on the pool and its result is silently discarded. Unlike
dropping a `JoinHandle` (which at least triggers a drain), there is no cleanup signal and
no way to know whether the work succeeded or failed. For any operation with observable
side effects or a result you care about, always `co_await` the handle.

```cpp
// BAD — result silently discarded; no way to know if it succeeded
spawn_blocking([]() { return write_file(); });  // [[nodiscard]] warning

// GOOD
auto result = co_await spawn_blocking([]() { return write_file(); });
```

### SC.3 — Always call `drain()` on a `JoinSet` unless cancellation of pending tasks is intentional

**Reason:** `JoinSet` owns its children's `JoinHandle`s. When the `JoinSet` is destroyed,
those handles are destroyed, each cancelling its task and registering it as a pending
child of the enclosing coroutine scope.

Cancel-on-drop is the deliberate default behaviour. If a `JoinSet` were to implicitly
drain on destruction instead, any accidental drop — an early `co_return`, an exception
propagating through the scope, an early return from a helper function — would silently
block the owning task until every child finished, potentially indefinitely if any child
is long-running or stuck. Cancellation is the safe default: it bounds the lifetime of
the `JoinSet`'s children to the lifetime of the `JoinSet` itself, and no task can be
held open longer than the caller intended.

The corollary is that an accidental drop before `drain()` cancels in-progress work
silently. Be aware of all paths that can destroy a `JoinSet` before `drain()` is called.

```cpp
// Intended early exit — unfinished tasks are cancelled
Coro<void> process(std::vector<Item>& items) {
    co_await co_invoke([&]() -> Coro<void> {
        JoinSet<void> js;
        for (auto& item : items)
            js.spawn(process_item(item));
        if (error_detected)
            co_return;  // js dropped here → all unfinished tasks cancelled
        co_await js.drain();
    });
}
```

---

## Error Handling

### EH.1 — Return `std::expected<T, E>` for fallible operations; throw only at boundaries

**Reason:** Channel closure, connection reset, and buffer-full conditions are normal
control flow in many programs — not exceptional events. Forcing callers into `try/catch`
for routine outcomes obscures the control flow and encourages catch-all handlers that
discard error detail. `std::expected` makes the success and failure paths equally visible
in the code and allows callers to check, transform, and propagate errors with standard
functional-style operations. `.value()` provides the exception escape hatch for call sites
where `try/catch` is more natural or where interoperating with exception-based code.

```cpp
// GOOD — explicit error check
auto r = co_await rx.recv();
if (!r) {
    handle_closed_channel(r.error());
    co_return;
}
process(*r);

// ALSO GOOD — propagate as exception where appropriate
try {
    process((co_await rx.recv()).value());  // throws ChannelError on failure
} catch (const ChannelError& e) { ... }
```

### EH.2 — Do not send exceptions through channels; use `channel<expected<T, E>>`

**Reason:** There is no mechanism to send an exception through a channel. If you capture
`std::exception_ptr` and send it as a raw value, the receiver sees two overlapping error
signals — the transport error from `recv()` returning `std::unexpected(ChannelError)`, and
the application error embedded in the value — with no clean way to distinguish them.
Using `channel<std::expected<T, E>>` gives each layer a distinct type and a clear meaning.

See **CH.3** for the full pattern.

### EH.3 — Exceptions in child tasks do not propagate automatically; collect them explicitly

**Reason:** An exception thrown inside a spawned task is captured as `std::exception_ptr`
and stored in the task's state. It is not rethrown until someone `co_await`s the
`JoinHandle`. If you never await the handle (e.g. you detach it, or the `JoinSet` drain
finishes and you discard the results), the exception is silently swallowed. This differs
from the behaviour programmers may expect from threads (`std::async` rethrows on
`future::get`) and requires explicit collection.

```cpp
// Exception is stored; not thrown yet
auto h = spawn(might_throw());

// Exception rethrown here
co_await h;  // throws if the task failed

// With JoinSet — exception rethrown on next() or drain()
JoinSet<int> js;
js.spawn(might_throw());
try {
    while (auto r = co_await next(js))
        use(*r);
} catch (const MyError& e) {
    // task's exception rethrown here
}
```

---

## ISR Safety (MCU platforms — `CORO_PICO` only)

On MCU platforms (`CORO_PICO` builds), coroutine tasks sometimes need to respond to
hardware interrupts. Nearly every coro operation touches `shared_ptr` ref-counts, atomic
scheduling state, or `detail::Mutex` — on Cortex-M0+ (RP2040/RP2350) all of these route
through a global spin-lock that disables IRQs, making them deadlock-prone from ISR context.
The rules below define the safe boundary between ISR code and the coroutine executor.

See `doc/isr_safety.md` for a detailed explanation of why these restrictions exist.

### IS.1 — Only call `signal_from_isr()` or `send_from_isr()` from an interrupt handler

**Reason:** These are the only two coro API calls explicitly designed and verified to be
ISR-safe on Cortex-M0+. They write a `volatile` flag (and, for `IsrChannel<T>`, a value
with a `__DMB()` release fence) and return immediately — no spin-lock, no allocation, no
`shared_ptr`. Everything else — `spawn`, `Event::set`, channel `send`, `wake()`, any
`shared_ptr` copy — is undefined behavior from ISR context, regardless of whether it
appears to work on a specific hardware configuration.

```cpp
coro::IsrEvent g_dma_done;

void dma_irq_handler() {
    dma_irqn_acknowledge_channel(0, MY_DMA_CHANNEL);
    g_dma_done.signal_from_isr();   // GOOD — ISR-safe

    // BAD — undefined behavior from ISR context on RP2040:
    // g_event.set();               // touches std::atomic + mutex
    // co_await_tx.send(value);     // std::atomic spin-lock → deadlock
    // coro::spawn(some_task());    // allocation + shared_ptr
}
```

### IS.2 — Keep interrupt handlers minimal: write a flag or value and return immediately

**Reason:** The ISR/executor protocol is designed around a strict division of labor: the
ISR does one write and exits; the executor discovers the result on its next polling pass
and schedules the coroutine normally. Any additional work in the ISR — branching, I/O,
complex logic — increases interrupt latency and raises the risk of calling into unsafe
API territory. The entire ISR body should typically be two or three lines.

```cpp
// GOOD — minimal; write one value and return
void uart_rx_isr() {
    g_uart_channel.send_from_isr(uart_read_byte());
}

// BAD — too much work in the ISR; increased latency and risk
void uart_rx_isr() {
    uint8_t byte = uart_read_byte();
    if (byte == FRAME_START) {
        g_frame_started = true;
        // ... any coro call here is UB ...
    } else if (g_frame_started) {
        g_uart_channel.send_from_isr(byte);
    }
}
```

Push any non-trivial logic into the waiting coroutine, which runs in executor context
where all coro operations are safe.

### IS.3 — Declare `IsrEvent` and `IsrChannel<T>` at static or class scope; never as coroutine locals

**Reason:** The ISR and the waiting coroutine share the same flag object. Both must be
alive for the entire communication window: an interrupt can fire at any time, including
after the coroutine has returned or been cancelled. A coroutine-local `IsrEvent` or
`IsrChannel<T>` is destroyed when the coroutine exits — any ISR that fires after that
point writes to freed memory.

```cpp
// GOOD — global scope; outlives all coroutines and all ISRs
coro::IsrChannel<uint32_t> g_dma_result;

// ALSO GOOD — class member; tied to the owning object's lifetime
struct DmaController {
    coro::IsrChannel<uint32_t> transfer_result;
    void install_isr();
    coro::Coro<uint32_t> run_transfer();
};

// BAD — destroyed when the coroutine returns or is cancelled
coro::Coro<void> bad() {
    coro::IsrEvent local_event;          // DO NOT DO THIS
    install_isr_handler(&local_event);
    co_await local_event.wait();
    // local_event destroyed here; ISR may still fire after this point
}
```

### EH.4 — Handle `ChannelError::Disconnected` explicitly in long-running producers

**Reason:** When all receivers of a channel have been dropped, the channel is closed from
the consumer side. Any subsequent `send()` will return a `Disconnected` error. In
server-style code where producers run until the receiver shuts them down, this is the
primary shutdown signal — it is the async equivalent of writing to a closed pipe. Treating
it as an unexpected error, logging it, or propagating it up as an exception is incorrect;
it should be a clean exit.

```cpp
Coro<void> producer(MpscSender<int> tx) {
    for (int i = 0; ; ++i) {
        auto r = co_await tx.send(i);
        if (!r) {
            // Receiver dropped — normal shutdown
            co_return;
        }
    }
}
```
