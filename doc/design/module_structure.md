# Module Structure

## Overview

The public headers under `include/coro/` are organized into subfolders that mirror Tokio's
top-level module boundaries. This makes the intended scope of each type clear and gives future
features an obvious home.

```
include/coro/
│
│  Flat — foundational surface API
│
├── coro.h              Coro<T> coroutine return type
├── coro_stream.h       CoroStream<T> async generator return type
├── co_invoke.h         co_invoke() helper — wraps a lambda as a Coro<T>
├── future.h            Future concept + NextFuture adapter
├── stream.h            Stream concept + next() helper
│
│  Submodules
│
├── runtime/            tokio::runtime — executor and event loop
│   ├── runtime.h
│   ├── executor.h
│   ├── single_threaded_uv_executor.h   libuv I/O reactor + task queue (dedicated thread)
│   ├── current_thread_executor.h       polling executor — runs on calling thread (MCU)
│   ├── single_threaded_executor.h
│   ├── work_sharing_executor.h
│   ├── work_stealing_executor.h
│   └── uv_future.h         UvCallbackResult / UvFuture — libuv callback bridges
│
├── task/               tokio::task — task spawning and handles
│   ├── join_handle.h
│   ├── join_set.h          JoinSet<T> — dynamic collection of homogeneous tasks
│   ├── stream_handle.h     StreamHandle<T> — consumer end of a spawned stream
│   ├── spawn_blocking.h    spawn_blocking() + BlockingHandle + BlockingPool
│   └── spawn_builder.h     SpawnBuilder + StreamSpawnBuilder
│
├── sync/               tokio::sync — synchronization primitives and channels
│   ├── broadcast.h         broadcast_channel — multi-producer, multi-consumer, lag-detecting
│   ├── cancellation_token.h
│   ├── channel_error.h     SendError / RecvError shared by all channel types
│   ├── event.h             Event — one-shot signal (any-thread set, coroutine wait)
│   ├── join.h              join() — wait for a fixed heterogeneous set of futures
│   ├── mpsc.h              mpsc_channel — bounded multi-producer single-consumer
│   ├── oneshot.h           oneshot_channel — single-value, single-use channel
│   ├── select.h            select() — race futures, return first ready
│   ├── sleep.h             sleep_for() / SleepFuture
│   ├── isr_event.h         IsrEvent / IsrChannel<T> — ISR-to-coroutine primitives (CORO_PICO only)
│   ├── timeout.h           timeout() — race any future against a deadline
│   └── watch.h             watch_channel — single latest-value, multi-consumer
│
├── io/                 Async I/O — network streams, WebSocket, OS signals
│   ├── tcp_stream.h        TcpStream — async TCP connection
│   ├── ws_stream.h         WsStream — async WebSocket client connection
│   ├── ws_listener.h       WsListener — async WebSocket server acceptor
│   └── signal.h            signal() / signal_stream() — async OS signal delivery (uv_signal_t)
│
└── detail/             Internal plumbing — not intended for direct user inclusion
    ├── poll_result.h       stable API for custom Future/Stream implementors
    ├── waker.h             stable API for custom Future/Stream implementors
    ├── context.h           stable API for custom Future/Stream implementors
    ├── coro_scope.h        purely internal — coroutine lifetime / child tracking
    ├── future_awaitable.h  purely internal — may change without notice
    ├── intrusive_list.h    purely internal — lock-free intrusive list
    ├── task.h              purely internal
    ├── task_state.h        purely internal
    └── work_stealing_deque.h  purely internal — Chase-Lev work-stealing deque

src/
├── runtime/
│   ├── runtime.cpp
│   ├── executor.cpp
│   ├── single_threaded_uv_executor.cpp
│   ├── single_threaded_executor.cpp
│   ├── current_thread_executor.cpp
│   ├── work_sharing_executor.cpp
│   └── work_stealing_executor.cpp
├── task/
│   ├── task.cpp                 detail::Task + detail::TaskBase
│   ├── waker.cpp                detail::Waker + Context
│   ├── blocking_pool.cpp
│   └── context.cpp              t_current_coro + CoroutineScope helpers
├── sync/
│   └── cancellation_token.cpp
├── io/
│   ├── tcp_stream.cpp
│   ├── tcp_listener.cpp
│   ├── ws_stream.cpp
│   ├── ws_listener.cpp
│   ├── file.cpp
│   ├── pipe.cpp
│   ├── signal.cpp
│   ├── circular_byte_buffer.cpp
│   └── lwip/                    Pico lwIP TCP backend (CORO_PICO)
│       ├── tcp_stream_lwip.cpp
│       └── tcp_listener_lwip.cpp
└── pico/
    └── hal/dma.cpp              RP2040 DMA helpers (CORO_PICO)
```

## Placement rules for new headers

### `coro.h` / `coro_stream.h` / `future.h` / `stream.h` — flat

These are kept at the top level because they are the foundational layer that every module
depends on. `coro.h` and `coro_stream.h` are the primary user-facing entry points (users
declare functions returning `Coro<T>` or `CoroStream<T>`). `future.h` and `stream.h` define
the concepts that appear in API signatures and template constraint errors, and `stream.h`
exports the `next()` helper that users call in `co_await` loops.

### `runtime/` — executor and event loop

Anything that drives tasks to completion or owns I/O infrastructure goes here. This includes:
- The `Runtime` class (entry point for `block_on` and `spawn`)
- The abstract `Executor` interface
- Concrete executor implementations: `SingleThreadedExecutor`, `WorkSharingExecutor`, `WorkStealingExecutor`
- `SingleThreadedUvExecutor` — libuv event loop + task queue on a dedicated thread
- `CurrentThreadExecutor` — polling executor for MCU targets; runs on the calling thread
- `uv_future.h` — `UvCallbackResult` / `UvFuture` bridge libuv callbacks to coroutines

### `task/` — task spawning and handles

Types that represent the *handle* to a running or completed task, and the builders used to
configure and submit tasks. This includes:
- `JoinHandle<T>` — `co_await` to retrieve a spawned task's result
- `JoinSet<T>` — fan out many homogeneous tasks; collect results in completion order
- `StreamHandle<T>` — consumer end of the bounded channel that backs `spawn(stream)`
- `SpawnBuilder` — returned by `build_task()`, configure name/buffer before calling `.spawn(f)`
- `BlockingHandle<T>` / `spawn_blocking()` — run blocking code on a dedicated thread pool

### `sync/` — synchronization primitives and channels

Types that coordinate between concurrently running tasks. This includes:
- `Event` — one-shot signal; `set()` from any thread, `co_await wait()` in a coroutine
- **Channels**: `oneshot` (single-value), `mpsc` (bounded multi-producer), `watch` (latest-value multi-consumer), `broadcast` (multi-producer multi-consumer, lag-detecting)
- **Combinators**: `join()`, `select()`, `timeout()`, `sleep_for()`
- `CancellationToken` — cooperative cancellation signal passed via `Context`
- `IsrEvent` / `IsrChannel<T>` — ISR-to-coroutine primitives; MCU platforms only (`CORO_PICO`)

### `io/` — async I/O

Types that provide async access to network resources. I/O types are built on top of
`SingleThreadedUvExecutor` (the libuv reactor in `runtime/`) and satisfy `Future` or `Stream`.

- `TcpStream` — async TCP connection
- `WsStream` — async WebSocket client connection
- `WsListener` — async WebSocket server acceptor
- `signal()` / `signal_stream()` — async OS signal delivery via libuv's `uv_signal_t`;
  see `doc/design/signal_handling.md`

### `detail/` — internal plumbing and low-level extension points

Headers in `detail/` fall into two sub-categories:

**Stable API for advanced users** (custom `Future`/`Stream` implementors and I/O integrators):
- `poll_result.h` — `PollResult<T>`, the return type of every `poll()` / `poll_next()` call
- `waker.h` — `Waker` abstract base; subclass this to integrate with external event sources
- `context.h` — `Context`, passed to every `poll()` call; carries the current `Waker`

These are in `detail/` because typical application code never includes them directly — they
are only needed when writing custom futures or integrating a new I/O backend. They are
**stable API**: their interfaces will not change without a compatibility note.

**Truly internal** (implementation details; do not include directly):
- `coro_scope.h` — coroutine lifetime tracking and child task management
- `future_awaitable.h` — C++20 awaiter adapter; part of the `co_await` machinery
- `intrusive_list.h` — intrusive linked list used by waiter queues
- `task.h` — type-erased `Task` wrapper held by the executor
- `task_state.h` — shared state between a running `Task` and its `JoinHandle`
- `work_stealing_deque.h` — Chase-Lev double-ended queue for the work-stealing executor

## Conventions

- Headers in `detail/` are included transitively by the public headers that need them. Users
  should not `#include` them directly unless they are implementing a custom `Future`, `Stream`,
  or I/O backend.
- Each new feature gets its own header in the appropriate subfolder; do not add declarations
  to existing unrelated headers.
- If a new type spans two modules (e.g. a channel has both a sender and a receiver), place both
  in the module that matches its primary user-visible role (`sync/` for channels).
