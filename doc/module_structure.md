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
├── future.h            Future concept + NextFuture adapter
├── stream.h            Stream concept + next() helper
│
│  Submodules
│
├── runtime/            tokio::runtime — executor and event loop
│   ├── runtime.h
│   ├── executor.h
│   └── single_threaded_executor.h
│
├── task/               tokio::task — task spawning and handles
│   ├── join_handle.h
│   └── spawn_builder.h     (SpawnBuilder + StreamSpawnBuilder)
│
├── sync/               tokio::sync — synchronization primitives
│   ├── synchronize.h
│   ├── stream_handle.h     (bounded channel consumer end)
│   └── cancellation_token.h
│
└── detail/             Internal plumbing — not intended for direct user inclusion
    ├── poll_result.h       stable API for custom Future/Stream implementors
    ├── waker.h             stable API for custom Future/Stream implementors
    ├── context.h           stable API for custom Future/Stream implementors
    ├── future_awaitable.h  purely internal — may change without notice
    ├── task.h              purely internal
    └── task_state.h        purely internal
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
- Concrete executor implementations (single-threaded, future multi-threaded work-stealing)
- libuv event loop integration (when added)

### `task/` — task spawning and handles

Types that represent the *handle* to a running or completed task, and the builders used to
configure and submit tasks. This includes:
- `JoinHandle<T>` — `co_await` to retrieve a spawned task's result
- `SpawnBuilder<F>` / `StreamSpawnBuilder<S>` — returned by `spawn()`, configured before `submit()`

### `sync/` — synchronization primitives

Types that coordinate between concurrently running tasks. This is the future home of channels
(mpsc, oneshot), mutexes, and other inter-task communication. Current members:
- `Synchronize` — structured concurrency scope; guarantees all child tasks finish before the
  parent continues
- `StreamHandle<T>` — consumer end of the bounded channel that backs `spawn(stream)`
- `CancellationToken` — cooperative cancellation signal passed via `Context`

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
- `future_awaitable.h` — C++20 awaiter adapter; part of the `co_await` machinery
- `task.h` — type-erased `Task` wrapper held by the executor
- `task_state.h` — shared state between a running `Task` and its `JoinHandle`

## Conventions

- Headers in `detail/` are included transitively by the public headers that need them. Users
  should not `#include` them directly unless they are implementing a custom `Future`, `Stream`,
  or I/O backend.
- Each new feature gets its own header in the appropriate subfolder; do not add declarations
  to existing unrelated headers.
- If a new type spans two modules (e.g. a channel has both a sender and a receiver), place both
  in the module that matches its primary user-visible role (`sync/` for channels).
