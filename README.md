# coro

A C++20 coroutines library for multi-threaded asynchronous task synchronization and I/O,
heavily inspired by Rust's async model and the [Tokio](https://tokio.rs) runtime.

Coroutines let you write concurrent code that reads like sequential code — suspend at
`co_await`, resume when the event fires, no callback hell, no manual state machines.

## Requirements

- C++23
- CMake
- [Conan](https://conan.io) (recommended for dependency management)

```bash
conan install . --build=missing -s:h build_type=Release
cmake --preset conan-release
cd build/Release && make
```

## Documentation

- [Introduction](doc/index.md)
- [Getting Started Guide](doc/getting_started.md)
- [Library Usage Guidelines](doc/guidelines.md)

### Internal Design Details

- [Futures and Streams](doc/future_and_stream.md)
- [Tasks, Wakers, and Context](doc/waker_and_context_propagation.md)
- [Tasks, Executors, and Runtime](doc/task_and_executor.md)
- [Executor Design](doc/executor_design.md) · [Work-Stealing Scheduler](doc/work_stealing_executor.md)
- I/O: [I/O Coroutines](doc/io_coroutine.md) · [libuv Integration](doc/libuv_integration.md) · [WebSocket Stream](doc/websocket_stream.md) · [PollStreams](doc/poll_streams.md)
- Synchronization: [Coroutine Scope](doc/coroutine_scope.md) · [JoinSet](doc/join_set.md) · [Channels](doc/channels.md) · [Select](doc/select.md)
- [Module Structure](doc/module_structure.md)

### Examples

- [Examples](doc/examples.md)

### Versioning and Releases

coro follows [Semantic Versioning](https://semver.org) via annotated git tags on `master`
(`vX.Y.Z`) — a tag is the sole trigger for a release. During the current pre-1.0 stage,
additive API changes may ship as patch releases rather than minor ones (see the doc below
for why). See [Versioning and Releases](doc/versioning.md) for the full scheme: how
prerelease/dev-build versions are derived, release candidates, ABI stability, and the
release workflow.

### Roadmap

- [Roadmap and Future Plans](doc/roadmap.md)
