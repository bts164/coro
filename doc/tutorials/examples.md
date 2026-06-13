# Examples

Worked examples showing common use cases for the `coro` library. Each example is a
self-contained program under `examples/` that can be built with the rest of the project.

**Status legend:** `[ TODO ]` — not yet written · `[ DRAFT ]` — file exists, exposition only · `[ DONE ]` — implemented and working

---

## Streams

| Status | Example | File | Description |
|---|---|---|---|
| `[ DRAFT ]` | Fibonacci generator | `examples/io/fibonacci.cpp` | `CoroStream<int>` yielding a Fibonacci sequence lazily; consumer iterates with `co_await next()`. |

---

## I/O

| Status | Example | File | Description |
|---|---|---|---|
| `[ DRAFT ]` | TCP echo server | `examples/io/tcp_echo_server.cpp` | Accept loop that spawns a task per connection echoing bytes back. Exposition only — `TcpListener` is not yet implemented. |
| `[ DRAFT ]` | TCP echo client | `examples/io/tcp_echo_client.cpp` | Connects to the TCP echo server, sends a message, and prints the reply. Exposition only — does not compile. |
| `[ DONE ]`  | WebSocket echo server | `examples/io/ws_echo_server.cpp` | WebSocket accept loop using `WsListener` + `WsStream`; echoes messages back to each client. |
| `[ DONE ]`  | WebSocket echo client | `examples/io/ws_echo_client.cpp` | Connects to the WebSocket echo server via `WsStream`, sends a message, and prints the reply. |

---

## Not yet written

The following examples are planned but the files do not exist yet:

| Example | File | Description |
|---|---|---|
| Hello coroutine | `examples/hello_coro.cpp` | `block_on` a single coroutine that returns a value. Shows `Coro<T>`, `Runtime`, and `co_return`. |
| Sleep and wake | `examples/sleep_and_wake.cpp` | `co_await sleep_for(500ms)` then print elapsed time. Exercises the libuv timer path end-to-end. |
| Spawn tasks | `examples/spawn_tasks.cpp` | Spawn several tasks with `spawn()`, collect `JoinHandle`s, and `co_await` each result. |
| JoinSet fan-out | `examples/join_set.cpp` | Use `JoinSet` to spawn a dynamic number of tasks and collect results as they complete. |
| Timeout | `examples/timeout.cpp` | Wrap a slow task with `co_await timeout(200ms, slow_op())`. |
| spawn_blocking | `examples/spawn_blocking.cpp` | Run a blocking computation on the `BlockingPool` without starving the executor. |
| Producer / consumer | `examples/producer_consumer.cpp` | One task writes into an `mpsc` channel; another reads from it. Classic pipeline pattern. |
| Oneshot rendezvous | `examples/oneshot_rendezvous.cpp` | Two tasks synchronize on a single value via `oneshot`. |
| Watch config | `examples/watch_config.cpp` | One task periodically updates a `watch` channel; multiple readers react to changes. |
| Async generator | `examples/async_generator.cpp` | A `CoroStream<int>` yielding a Fibonacci sequence; consumer iterates with `co_await next()`. |
