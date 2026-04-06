# Examples

Worked examples showing common use cases for the `coro` library. Each example is a
self-contained program under `examples/` that can be built with the rest of the project.

**Status legend:** `[ TODO ]` — not yet written · `[ DRAFT ]` — stubbed/exposition only · `[ DONE ]` — implemented and working

---

## Basics

Minimal examples covering the core coroutine return type, runtime setup, and timers.

| Status | Example | File | Description |
|---|---|---|---|
| `[ TODO ]` | Hello coroutine | `examples/hello_coro.cpp` | `block_on` a single coroutine that returns a value. Shows `Coro<T>`, `Runtime`, and `co_return`. |
| `[ TODO ]` | Sleep and wake | `examples/sleep_and_wake.cpp` | `co_await sleep_for(500ms)`, then prints elapsed time. Exercises the libuv timer path end-to-end. |

---

## Concurrency

Examples that spawn multiple tasks and coordinate their results.

| Status | Example | File | Description |
|---|---|---|---|
| `[ TODO ]` | Spawn tasks | `examples/spawn_tasks.cpp` | Spawns several tasks with `runtime.spawn()`, collects `JoinHandle`s, and `co_await`s each result. Shows parallel work on a multi-threaded executor. |
| `[ TODO ]` | JoinSet | `examples/join_set.cpp` | Uses `JoinSet` to spawn a dynamic number of tasks and collect results as they complete. |
| `[ TODO ]` | Timeout | `examples/timeout.cpp` | Wraps a slow task with `co_await timeout(200ms, slow_op())`. Shows cancellation via `CancellationToken` and the `timeout` combinator. |

---

## Channels

Examples using `mpsc`, `oneshot`, and `watch` channels for inter-task communication.

| Status | Example | File | Description |
|---|---|---|---|
| `[ TODO ]` | Producer / consumer | `examples/producer_consumer.cpp` | One task writes into an `mpsc` channel; another reads from it. Classic pipeline pattern. |
| `[ TODO ]` | Oneshot rendezvous | `examples/oneshot_rendezvous.cpp` | Two tasks synchronize on a single value via `oneshot`. Demonstrates "fire and await result" coordination. |
| `[ TODO ]` | Watch config | `examples/watch_config.cpp` | One task periodically updates a `watch` channel; multiple readers react to changes. |

---

## Streams

Examples using `CoroStream<T>` async generators and stream composition.

| Status | Example | File | Description |
|---|---|---|---|
| `[ TODO ]` | Async generator | `examples/async_generator.cpp` | A `CoroStream<int>` yielding a Fibonacci sequence lazily; consumer iterates with `co_await next()`. |
| `[ TODO ]` | Stream pipeline | `examples/stream_pipeline.cpp` | Chains a generator through map/filter operations and collects results, showing stream composition. |

---

## I/O

Examples using the libuv-backed `IoService` for async network and file I/O.

| Status | Example | File | Description |
|---|---|---|---|
| `[ DRAFT ]` | TCP echo server | `examples/io/tcp_echo_server.cpp` | Accept loop that spawns a task per connection echoing bytes back. Exposition only — does not compile until `TcpStream` / `TcpListener` are implemented. |
| `[ DRAFT ]` | TCP echo client | `examples/io/tcp_echo_client.cpp` | Connects to the TCP echo server, sends a message, and prints the reply. Exposition only — does not compile until `TcpStream` is implemented. |
| `[ DRAFT ]` | WebSocket echo server | `examples/io/ws_echo_server.cpp` | WebSocket accept loop that echoes messages back to each client. Exposition only — does not compile until `WsListener` is implemented. |
| `[ DRAFT ]` | WebSocket echo client | `examples/io/ws_echo_client.cpp` | Connects to the WebSocket echo server, sends a message, and prints the reply. Compiles once Phase 3 (`WsStream`) is complete. |
