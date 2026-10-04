# coro

A C++ coroutines library for asynchronous task synchronization and I/O, heavily inspired
by Rust's async model and the [Tokio](https://tokio.rs) runtime. It scales from
cooperative multitasking on a single thread all the way to a fully multi-threaded
work-stealing executor, and the runtime is lightweight enough to run on bare-metal
microcontrollers with no RTOS required — a working Raspberry Pi Pico port ships with the
library.

**Documentation: <https://bts164.github.io/coro/>** — start with the
[Getting Started guide](https://bts164.github.io/coro/getting_started/), or keep the
[cheat sheet](https://bts164.github.io/coro/cheatsheet/) open while you work.

## What are coroutines

Readers familiar with `async`/`await` in Python, JavaScript, Rust or Kotlin, or with Go's
goroutines, will recognize the pattern immediately. This library brings the same model to
C++ using the coroutine support introduced in C++20.

A coroutine is a function that can **suspend itself mid-execution** — pausing at a
`co_await` expression while it waits for some event — and then **resume from exactly where
it left off** when that event fires. The result is concurrent code whose execution flow is
as easy to reason about as sequential code.

```cpp
coro::Coro<std::string> fetch(std::string url) {
    TcpStream conn = co_await TcpStream::connect(url, 80);  // suspends here
    co_await conn.write(make_request(url));                  // suspends here
    auto response = co_await conn.read(buffer);              // suspends here
    co_return parse(response);
}
```

This pays off in programs that spend most of their time waiting on many things at once —
sockets, timers, messages from other tasks. It does not make CPU-bound work any faster;
there, coro's role is to keep the compute loop fed.

Coroutines do not run themselves. A **runtime** schedules them and resumes them as events
fire, and `Runtime::block_on()` is the entry point from synchronous code:

```cpp
int main(int argc, char* argv[]) {
    coro::Runtime rt;
    return rt.block_on(async_main(argc, argv));
}
```

### Tasks vs. threads

*A task is a coroutine scheduled and driven by the runtime executor.* The crucial
difference from OS threads is what happens while a task waits:

| | OS Thread | Task |
|---|---|---|
| Waiting for I/O | Thread **blocks** — OS parks it, stack stays allocated | Task **suspends** — frame stays on the heap so it can resume later; control returns to the runtime immediately |
| Switching cost | OS kernel context switch (~microseconds) | Executor resumes next task (~nanoseconds) |
| Memory per unit | ~1–8 MB stack (OS default) | A few hundred bytes on the heap |
| Scheduling | Preemptive — OS can interrupt at any time | Cooperative — suspends only at `co_await` points |
| Practical scale | Thousands | Hundreds of thousands |


## Key features

- **`Coro<T>` / `CoroStream<T>`** — async function and async generator return types; compose with `co_await` and `co_yield`.
- **`spawn()` / `JoinHandle` / `JoinSet`** — spawn background tasks, await their results or cancel them, or fan out to many tasks and collect results in completion order.
- **Channels** — thread-safe, typed, async channels for inter-task communication:
    - **`oneshot`** — single-use, one value, one sender, one receiver; sender is synchronous.
    - **`mpsc`** — bounded, backpressured queue; multiple producers, one consumer.
    - **`watch`** — single latest value, multiple senders, multiple receivers.
    - **`broadcast`** — every receiver sees every message; multiple senders, multiple receivers.
- **Concurrent combinators** — `join` (wait for all), `select` (first wins), `timeout` (deadline), `sleep_for` (non-blocking sleep).
- **`spawn_blocking()`** — run blocking code on a dedicated thread pool without starving the executor.
- **Async I/O** — `File`, `TcpStream`, `TcpListener`, `WsStream`, and `WsListener` for async file, TCP, and WebSocket I/O. TCP, UDP, pipes and signals run on coro's own epoll I/O driver; files and DNS (`lookup_host`) run on the blocking pool; WebSockets run libwebsockets on its own service threads.
- **Multi-threaded executor** — tasks are distributed across worker threads automatically.
- **MCU support** — `CurrentThreadExecutor` runs the full task graph on the calling thread; ships with a working Raspberry Pi Pico port.


## Quick example

The following is a common pattern in connection-based server communication: poll two
redundant servers for a long-running result, send keepalives while waiting to detect
disconnections, and enforce an overall deadline. The thread-and-callback version requires
four things that have nothing to do with the logic:

- A state machine to track which phase you're in
- A mutex protecting the shared result
- A dedicated timer thread for keepalives
- A cancellation flag carefully threaded through every layer — with the constant risk that
  a blocking call somewhere never checks it

With coroutines, the structure maps directly to the intent. The three concurrent concerns
(two redundant `poll_status` calls and `keepalive`) are just three branches of a `select`.
The deadline is one `timeout` wrapper. Cancellation is intrinsic — every `co_await` is
already an exit point.

Under the hood the same four pieces are still there — the coroutine frame *is* the state
machine (generated by the compiler from your sequential code), each result is handed
back to the coroutine awaiting it instead of being written to shared memory behind a
mutex, the executor drives the timer callbacks, and every `co_await` is a built-in
cancellation check. Whatever locking that hand-off needs lives inside the library; the
application has no shared state to protect and no synchronization primitives to get
right. You just never have to explicitly write any of it.

```cpp
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/join.h>
#include <coro/sync/select.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

// Placeholder types — substitute coro::TcpStream, a gRPC stub, WsStream, etc.
struct Connection {};
struct Result { bool ready; std::string value; };

coro::Coro<Result> poll_status(Connection& c, int request_id);
coro::Coro<void>   ping(Connection& c);

// Waits 500ms, then pings both connections concurrently with a 500ms deadline.
// Throws if either server is unreachable or does not respond in time.
coro::Coro<void> keepalive(Connection& primary, Connection& backup) {
    using namespace std::chrono_literals;
    co_await coro::sleep_for(500ms);
    auto r = co_await coro::timeout(500ms, coro::join(ping(primary), ping(backup)));
    if (r.index() != 0)
        throw std::runtime_error("keepalive timed out");
}

// Polls two redundant servers until one delivers a ready result.
coro::Coro<Result> poll_until_ready(Connection& primary, Connection& backup, int id) {
    using namespace std::chrono_literals;

    while (true) {
        // Race both servers against a keepalive tick.
        // select() drives all three concurrently; first to complete wins.
        // select() returns a variant — index() tells you which branch won.
        auto sel = co_await coro::select(
            poll_status(primary, id),   // branch 0
            poll_status(backup,  id),   // branch 1
            keepalive(primary, backup)  // branch 2: fires after 500ms of silence
        );

        if (sel.index() == 0 || sel.index() == 1) {
            // A server responded — unwrap whichever branch won.
            Result& r = sel.index() == 0
                ? std::get<0>(sel).value  // .value is the SelectBranch result field
                : std::get<1>(sel).value;

            if (r.ready)
                co_return r;

            co_await coro::sleep_for(100ms);  // not ready yet — poll again shortly
        }
        // branch 2: keepalive fired, connections verified — loop and poll again.
        // A ping failure or timeout throws out of select() before reaching here.
    }
}

coro::Coro<void> run(Connection& primary, Connection& backup) {
    using namespace std::chrono_literals;

    // Enforce an overall deadline across the entire poll loop.
    // When it fires, both in-flight requests and any pending keepalive drain cleanly.
    // timeout() returns a variant: index 0 = completed, index 1 = deadline fired.
    auto outcome = co_await coro::timeout(30s, poll_until_ready(primary, backup, 42));

    if (outcome.index() == 0)
        std::cout << std::get<0>(outcome).value.value << "\n";  // SelectBranch.value = Result
    else
        std::cout << "timed out\n";
}

int main() {
    coro::Runtime rt;
    Connection primary, backup;
    rt.block_on(run(primary, backup));
}
```

Structured cancellation composes for free at any granularity: spawn `run()` as a task and
drop the `JoinHandle` at any point — every `co_await` in the entire tree (poll loop,
keepalive, timeout) becomes a clean exit point automatically. No tokens to thread through
every layer, no risk of getting stuck at a blocking call that never checks the flag.

More worked examples: the [Getting Started guide](https://bts164.github.io/coro/getting_started/) builds a TCP
echo server and client step by step, and the finished programs are in
[examples/io](examples/io).

## Requirements

| | Supported and tested | Not supported today |
|---|---|---|
| Platform and compiler | GCC 13 on Linux; Raspberry Pi Pico (RP2040), bare metal | Windows / MSVC — the I/O driver is built on `epoll` |
| C++ standard | C++23 | C++20 should be possible with adapters for the few C++23 library types the API uses, such as `std::expected`, but is not regularly tested |
| Build | [Conan](https://conan.io) 2 and CMake 3.21 or later (3.31 for the tests) | Plain CMake with the dependencies installed by hand has never been confirmed to work |

Conan fetches or builds the library's dependencies ([libwebsockets](https://libwebsockets.org)
and, by default, [gperftools](https://github.com/gperftools/gperftools)). libwebsockets
also needs the `libcap` development package from the system:

```bash
sudo apt-get install build-essential git pkg-config libcap-dev python3-pip
pip install conan "cmake>=3.31"
```

The Conan profile has to select C++23. After `conan profile detect`, set this in
`~/.conan2/profiles/default`:

```ini
compiler.cppstd=gnu23
```

## Using coro in your project

coro is consumed as a Conan package. It is not on ConanCenter, so build it into your local
Conan cache from a checkout of a release tag:

```bash
git clone https://github.com/bts164/coro.git   # a full clone: the version comes from the git tags
cd coro
git checkout v0.1.1
conan create . --build=missing
```

Then require it from your project's `conanfile.py`:

```python
def requirements(self):
    self.requires("coro/0.1.1")
```

and link its target in your `CMakeLists.txt`:

```cmake
find_package(coro CONFIG REQUIRED)

add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE coro::coro)
```

[examples/io](examples/io) is a complete consumer project laid out exactly this way and is
the quickest starting point to copy from. To build against an untagged commit or a live
checkout instead, see [Versioning and Releases](https://bts164.github.io/coro/versioning/).

Package options, set with `-o` on the Conan command line:

| Option | Default | Effect |
|---|---|---|
| `shared` | `True` | Build a shared library; `False` builds a static one |
| `with_gperftools` | `True` | Link gperftools (tcmalloc) |
| `with_local_run_queue` | `True` | Lock-free per-worker run queue in `WorkStealingExecutor` |
| `with_sanitize` | `none` | `asan` (AddressSanitizer, LeakSanitizer and UBSan) or `tsan` (ThreadSanitizer) |

## Building from source

Only needed to work on the library itself or to run its tests.

### The library

```bash
conan install . --build=missing -s:h build_type=Release
cmake --preset conan-release
cmake --build --preset conan-release
```

`build_type` may be `Release`, `Debug` or `RelWithDebInfo`; the matching presets are
`conan-release`, `conan-debug` and `conan-relwithdebinfo`, and the output lands in
`build/<build_type>`.

### The tests

The tests are a separate Conan project under [test/](test) that consumes the coro package,
so coro is exported to the Conan cache first. They also need `arm-none-eabi-gcc` on the
`PATH` (`sudo apt-get install gcc-arm-none-eabi`), for one test that cross-assembles the
Pico context-switch code.

```bash
conan export .                 # coro itself
conan export test/libunicorn   # a test-only dependency kept in this repository
cd test
conan build . --build=missing
cd build/Release && ctest
```

For a sanitizer build, set `CORO_SANITIZE=asan` or `CORO_SANITIZE=tsan` in the environment
before those commands; it applies to both the library and the tests.
`docker/run-sanitizer-build.sh asan|tsan` runs the same thing in an Ubuntu 24.04 container
that mirrors CI.

### The examples

Each directory under [examples/](examples) is likewise its own Conan project. With coro in
the Conan cache (`conan create .` or `conan export .` as above):

```bash
cd examples/io
conan build . --build=missing

./build/Release/tcp_echo_server    # terminal 1: listens on 127.0.0.1:8080, Ctrl-C to stop
./build/Release/tcp_echo_client    # terminal 2: ten connections, replies written to results.txt
```

| Directory | Contents |
|---|---|
| [examples/io](examples/io) | TCP and WebSocket echo servers and clients; a `CoroStream` Fibonacci generator |
| [examples/grpc](examples/grpc) | gRPC servers running on `coro::Runtime` ([README](examples/grpc/README.md)) |
| [examples/pico](examples/pico) | Raspberry Pi Pico firmware: TCP echo, WS2812 LED control ([README](examples/pico/README.md)) |

### The documentation

The site is built with MkDocs and Doxygen:

```bash
sudo apt-get install doxygen graphviz
pip install -r requirements.txt
mkdocs serve    # http://127.0.0.1:8000
```

## Repository layout

```
include/coro/   public headers
src/            library implementation
test/           unit tests (separate Conan project)
examples/       example programs (separate Conan projects)
doc/            documentation sources for the site
cmake/ pico/    build support, including the Raspberry Pi Pico platform
docker/         container scripts that mirror CI
```

## Documentation

Everything is published at **<https://bts164.github.io/coro/>**:

- [Getting Started](https://bts164.github.io/coro/getting_started/) — step-by-step introductory tour of all major features with examples.
- [Cheat Sheet](https://bts164.github.io/coro/cheatsheet/) — the core API on a single printable page.
- [Library Usage Guidelines](https://bts164.github.io/coro/guidelines/) — C++ Core Guidelines style rules for writing correct, safe, and idiomatic code with this library.
- [Patterns](https://bts164.github.io/coro/notes/patterns/) — idiomatic solutions to recurring async programming problems.
- [Internal Design Details](https://bts164.github.io/coro/design/architecture/) — architecture, design decisions, and implementation reference.
- [Versioning and Releases](https://bts164.github.io/coro/versioning/) — how versions are derived from git tags, and what a release promises.
- [Roadmap](https://bts164.github.io/coro/roadmap/)

## License

[MIT](LICENSE)
