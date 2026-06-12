# Pico Port

Stripped-down port of the coro library to Raspberry Pi Pico W (RP2040/RP2350),
targeting the Pico SDK with `CYW43_ARCH_POLL` mode. Provides `CurrentThreadExecutor`,
`TcpStream`, `TcpListener`, `sleep_for`, and `timeout` — enough to run coroutines
with async TCP I/O, timers, and hardware IRQ integration on bare metal.

## Scope

| Included | Excluded |
|---|---|
| `Coro<T>` / `JoinHandle<T>` / `JoinSet<T>` — coroutine return types | `WorkStealingExecutor` / `WorkSharingExecutor` |
| `spawn()` — task spawning and joining | `File` / `WsStream` |
| `CurrentThreadExecutor` — single-threaded run loop | `spawn_blocking` (no thread pool) |
| `TcpStream` — async TCP client (lwIP backend) | |
| `TcpListener` — async TCP server (lwIP backend) | |
| `mpsc`, `oneshot`, `watch`, `select`, `join` — sync primitives | |
| `sleep_for` / `timeout` — timer-queue based, no ISR | |

All coroutine machinery in `include/coro/detail/` and `include/coro/sync/` is
portable C++20/23 with no platform dependencies and compiles as-is.

---

## Architecture

### Compile-time feature flags

| Flag | Effect |
|---|---|
| `CORO_PICO` | Selects `CurrentThreadExecutor`, bare-metal mutex stubs, `SleepFuture` timer queue, no libuv |
| `CORO_TCP_BACKEND_LWIP` | Selects lwIP TCP backend for `TcpStream` / `TcpListener` |

Both flags are set automatically by `cmake/platforms/pico.cmake`.

### How the existing library maps to the Pico SDK

| libuv backend | Pico SDK equivalent |
|---|---|
| `uv_run(UV_RUN_ONCE)` | `cyw43_arch_poll()` |
| `uv_tcp_t` | lwIP `tcp_pcb*` |
| `uv_timer_t` | `CurrentThreadExecutor` timer queue (min-heap + `time_us_64()`) |
| `uv_async_send()` doorbell | Not needed — single-threaded; ISR path uses critical sections |
| `UvCallbackResult` | `LwipCallbackResult` (no mutex; see below) |
| `SingleThreadedUvExecutor` (dedicated I/O thread) | `CurrentThreadExecutor` (runs on calling thread) |

### Event loop

`Runtime::block_on()` drives everything:

```mermaid
flowchart TD
    Start([block_on]) --> Check{state.terminated?}
    Check -->|yes| End([return])
    Check -->|no| Poll[poll_ready_tasks]
    Poll --> Timers[check_expired_timers]
    Timers --> WiFi[cyw43_arch_poll]
    WiFi --> Check
```

`cyw43_arch_poll()` processes pending WiFi chip events and fires any pending lwIP
callbacks (TCP recv, sent, err) synchronously on the calling thread before returning.
`check_expired_timers()` fires wakers for any `sleep_for` / `timeout` deadlines that
have passed since the last iteration.

---

## Synchronisation and ISR safety

### `detail::Mutex` — IRQ-disabling critical sections

On all non-Pico targets `detail::Mutex` / `detail::SharedMutex` are thin aliases for
`std::mutex` / `std::shared_mutex`. On Pico they are implemented as IRQ-disabling
critical sections via the Pico SDK's `save_and_disable_interrupts()` /
`restore_interrupts()`:

```cpp
class Mutex {
public:
    void lock()     { m_save = save_and_disable_interrupts(); }
    void unlock()   { restore_interrupts(m_save); }
    bool try_lock() { lock(); return true; }
private:
    uint32_t m_save = 0;
};
```

This is used uniformly for every mutex instance in the library — including channel
state, oneshot, watch, and the executor's ready queue.

**Tradeoff acknowledged:** disabling interrupts for every mutex operation is a stronger
guarantee than strictly necessary for purely intra-coroutine state (which is only ever
accessed from the executor thread). The overhead is negligible — `CPSID I` / `CPSIE I`
is 2 CPU cycles (~16 ns at 125 MHz) — and all critical sections protect only a handful
of instructions, so the interrupt-disabled window is imperceptibly short. The benefit is
uniform ISR safety: users are explicitly encouraged to call channel senders (e.g.
`OneshotSender::send()`) from IRQ handlers to bridge hardware events into coroutines,
and that pattern works correctly with any sync primitive without further thought.

### Recommended pattern: IRQ → coroutine via `oneshot`

The idiomatic way to await a hardware event (DMA completion, GPIO edge, UART RX, etc.)
without writing a custom `Future` is to use an `oneshot` channel with the sender called
from the IRQ handler:

```cpp
static coro::OneshotSender<void> g_dma_done_tx;

// IRQ handler — C linkage, called from hardware interrupt
extern "C" void dma_irq_handler() {
    dma_hw->ints0 = 1u << DMA_CHANNEL;
    g_dma_done_tx.send();   // wakes the awaiting coroutine; ISR-safe
}

// Coroutine
coro::Coro<void> do_dma_transfer() {
    auto [tx, rx] = coro::oneshot<void>();
    g_dma_done_tx = std::move(tx);

    dma_channel_set_irq0_enabled(DMA_CHANNEL, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(DMA_CHANNEL);

    co_await std::move(rx);   // parks until IRQ fires
}
```

`CurrentThreadExecutor::enqueue()` (called by `waker->wake()` inside `send()`) holds
`m_ready_mutex` — an IRQ-disabling `detail::Mutex` — only for the queue push, keeping
the interrupt-disabled window to a minimum.

### `LwipCallbackResult` — no mutex needed for lwIP callbacks

`LwipCallbackResult` (used for DNS resolution and TCP connect) stores a waker and a
result value but uses no mutex. This is safe because `CYW43_ARCH_POLL` guarantees that
all lwIP callbacks fire synchronously inside `cyw43_arch_poll()`, which runs on the
executor thread. There is no separate I/O thread and no concurrent access.

!!! warning "CYW43_ARCH_THREADSAFE_BACKGROUND incompatibility"
    If you switch to `CYW43_ARCH_THREADSAFE_BACKGROUND`, lwIP callbacks can arrive from
    core 1 or a background IRQ. `LwipCallbackResult` would then need a `detail::Mutex`
    guard around `waker` and `value`. The entire port is designed around `CYW43_ARCH_POLL`.

---

## File structure

```
cmake/platforms/
  pico.cmake                    Platform include: defines coro_pico STATIC library

include/coro/
  detail/mutex.h                Platform-portable mutex: critical sections on Pico,
                                std::mutex elsewhere
  pico/
    pico_executor.h             CurrentThreadExecutor class (timer queue, ready queue)
    pico_callback_result.h      PicoCallbackResult / PicoFuture — legacy; used by
                                older pico_tcp_stream.h. New code uses LwipCallbackResult.
  io/
    tcp_stream.h                TcpStream — dispatches to lwIP or libuv backend
    tcp_listener.h              TcpListener — dispatches to lwIP or libuv backend
    lwip/
      lwip_callback_result.h    LwipCallbackResult<Args...> + lwip_wait() helper
  sync/
    sleep.h                     SleepFuture — timer-queue based on Pico, libuv on desktop

src/
  pico_executor.cpp             CurrentThreadExecutor + timer queue implementation
  runtime.cpp                   Runtime::schedule_timer() (Pico gate)
  io/lwip/
    lwip_tcp_ctx.h              LwipTcpCtx internal struct (shared between stream + listener)
    tcp_stream_lwip.cpp         TcpStream lwIP implementation
    tcp_listener_lwip.cpp       TcpListener lwIP implementation

examples/pico/
  CMakeLists.txt                Standalone Pico SDK build
  lwipopts.h                    lwIP configuration (DHCP, TCP, NO_SYS=1)
  pico_tcp_echo_server.cpp      TCP echo server example
  pico_tcp_echo_client.cpp      TCP echo client example
  README.md                     Build instructions
```

---

## Component design

### `CurrentThreadExecutor`

Identical CAS state machine as `SingleThreadedExecutor`
(`Notified → Running → Idle / RunningAndNotified`). Key differences:

- **Single-threaded.** No remote injection queue. `enqueue()` pushes directly to
  `m_ready`, protected by `m_ready_mutex` (an IRQ-disabling critical section) so
  that wakers called from ISR handlers are safe.
- **No `std::condition_variable` in the run loop.** `wait_for_completion()` polls
  `state.terminated` in a loop rather than blocking on a condvar.
- **Timer queue.** A min-heap of `(deadline_us, waker)` pairs. `schedule_timer()`
  pushes an entry; `check_expired_timers()` fires wakers on each loop iteration.
  No hardware alarm or ISR is used — resolution is bounded by poll loop latency
  (sub-millisecond in practice).
- **`Runtime` is the public entry point.** Users call `rt.block_on(coro)`, not
  `CurrentThreadExecutor` methods directly. `CurrentThreadExecutor` is an internal implementation detail.

### `sleep_for` / `timeout` (Pico)

`SleepFuture` records a deadline in microseconds (via `time_us_64()`) and calls
`current_runtime().schedule_timer(deadline_us, waker)` on the first `poll()` where the
deadline hasn't passed. `check_expired_timers()` in the executor loop fires the waker
when the deadline is reached; the next `poll()` returns `PollReady`.

`timeout<F>` is implemented on top of `sleep_for` and works unchanged on Pico because
it only depends on `SleepFuture` and `select`.

### `LwipCallbackResult<Args...>`

One-shot bridge between an lwIP C callback and an awaiting coroutine. Stores a `Waker`
pointer set by `poll()` and an `optional<tuple<Args...>>` result set by `complete()`.
No mutex is needed (see [Synchronisation](#synchronisation-and-isr-safety) above).

```mermaid
sequenceDiagram
    participant C as coroutine
    participant L as lwIP
    participant E as CurrentThreadExecutor

    C->>C: allocate LwipCallbackResult on frame
    C->>L: start async op (tcp_connect, dns_gethostbyname, ...)
    C->>E: co_await lwip_wait(result) → suspend, store waker
    L-->>C: callback fires: result.complete(args...)
    Note over C: waker->wake() called
    E->>C: enqueue task → next poll() returns PollReady
```

### `LwipTcpCtx` — TCP stream shared state

Internal struct shared between a `TcpStream` and the three lwIP callbacks registered
on its PCB (`on_recv`, `on_sent`, `on_err`). Owned by `std::shared_ptr`.

```mermaid
stateDiagram-v2
    [*] --> Connected : accept() / connect() completes
    Connected --> Connected : on_recv (data appended to rx_buf)
    Connected --> Connected : on_sent (tx_waker woken)
    Connected --> EOF : on_recv(p == nullptr)
    Connected --> Errored : on_err (lwIP frees PCB before callback)
    EOF --> [*]
    Errored --> [*]
```

**Receive:** `on_recv` copies pbuf data into `rx_buf` (`std::vector<uint8_t>`),
calls `tcp_recved()` to open the window, and wakes `rx_waker`. `read()` drains up to
`buf.size()` bytes per call; multiple `on_recv` firings accumulate in `rx_buf`.

**Transmit:** `write()` loops calling `tcp_write(..., TCP_WRITE_FLAG_COPY)` in chunks
sized to `tcp_sndbuf()`. When the send buffer fills, it flushes with `tcp_output()` and
suspends on `tx_waker` until `on_sent` fires.

**Error:** `on_err` is called by lwIP after a fatal error; lwIP frees the PCB *before*
the callback returns, so `on_err` sets `pcb = nullptr`. All code that touches the PCB
checks `errored` or `pcb != nullptr` first.

**Accept race:** callbacks must be set up immediately in `on_accept`, not deferred to
when the application calls `accept()`. Data can arrive in the same `cyw43_arch_poll()`
pass that establishes the connection — registering `tcp_recv` in `on_accept` prevents
that data from being silently dropped.

### `TcpStream` destructor

Uses `tcp_close()` rather than `tcp_abort()`. `tcp_abort()` sends an RST and discards
the send buffer immediately; `tcp_close()` sends FIN and lets the peer receive any data
already in flight. `tcp_abort()` is used only as a fallback if `tcp_close()` returns
an error.

### TCP connect sequence

```mermaid
sequenceDiagram
    participant C as coroutine
    participant L as lwIP / cyw43
    participant E as CurrentThreadExecutor

    C->>L: dns_gethostbyname(host, dns_cb)
    C->>E: suspend (co_await lwip_wait(dns_result))
    L-->>C: dns_cb: dns_result.complete(addr)
    E->>C: wake → re-poll

    C->>L: tcp_connect(pcb, addr, port, conn_cb)
    C->>E: suspend (co_await lwip_wait(conn_result))
    L-->>C: conn_cb: conn_result.complete(ERR_OK)
    E->>C: wake → re-poll

    C->>L: register tcp_recv / tcp_sent / tcp_err on pcb
    C-->>C: co_return TcpStream(ctx)
```

---

## CMake integration

`cmake/platforms/pico.cmake` defines a single `coro_pico` STATIC library containing
all sources (portable core + executor + lwIP TCP). All sources are merged into one
archive to avoid circular linker ordering between the core (which references
`CurrentThreadExecutor`) and the executor (which references core symbols).

```cmake
# examples/pico/CMakeLists.txt pattern

cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(my_pico_project CXX C ASM)

set(PICO_CXX_ENABLE_EXCEPTIONS 1)    # must be set before pico_sdk_init()
pico_sdk_init()

set(CORO_ROOT /path/to/coro)
include(${CORO_ROOT}/cmake/platforms/pico.cmake)

# lwipopts.h must be visible when compiling coro_pico lwIP sources
target_include_directories(coro_pico PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(my_firmware main.cpp)
target_include_directories(my_firmware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(my_firmware PRIVATE coro_pico pico_stdlib pico_cyw43_arch_lwip_poll)
pico_enable_stdio_usb(my_firmware 1)
pico_add_extra_outputs(my_firmware)   # generates .uf2 / .bin / .hex
```

!!! note "SDK deps are PRIVATE to coro_pico"
    `pico_stdlib` and `pico_cyw43_arch_lwip_poll` are linked PRIVATE to `coro_pico`.
    They provide include paths for compiling the library sources but are not bundled
    into the archive. Applications must link them directly to avoid duplicate symbol
    errors (the SDK uses OBJECT libraries internally).

!!! note "PICO_BOARD"
    Set `PICO_BOARD=pico_w` (not the default `pico`) — the `pico` board has no CYW43
    WiFi chip and `cyw43_arch.h` will not be found. Set it as an environment variable
    or pass `-DPICO_BOARD=pico_w` to cmake.

`lwipopts.h` must be provided by the application project. Minimum recommended settings:

```c
#define NO_SYS                  1
#define MEM_LIBC_MALLOC         1
#define LWIP_ARP                1
#define LWIP_ETHERNET           1
#define LWIP_DHCP               1
#define LWIP_DNS                1
#define LWIP_TCP                1
#define LWIP_UDP                1
#define MEMP_NUM_TCP_PCB        8
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define TCP_MSS                 1460
#define TCP_SND_BUF             (4 * TCP_MSS)
#define TCP_WND                 (4 * TCP_MSS)
```

---

## Usage example

```cpp
#include <pico/stdlib.h>
#include <pico/cyw43_arch.h>
#include <lwip/netif.h>
#include <coro/runtime/runtime.h>
#include <coro/io/tcp_listener.h>
#include <coro/io/tcp_stream.h>
#include <coro/coro.h>
#include <coro/task/join_set.h>
#include <coro/sync/timeout.h>
#include <string>

using namespace coro;
using namespace std::chrono_literals;

static Coro<void> handle(TcpStream stream) {
    for (;;) {
        auto [n, buf] = co_await stream.read(std::string(4096, '\0'));
        if (n == 0) co_return;
        buf.resize(n);
        co_await stream.write(std::move(buf));
    }
}

static Coro<void> run_server() {
    TcpListener listener = co_await TcpListener::bind("0.0.0.0", 8080);
    JoinSet<void> sessions;
    for (int i = 0;;) {
        auto result = co_await coro::timeout(5s, listener.accept());
        if (result.index() != 0) {
            std::printf("still waiting...\n");
            continue;
        }
        sessions.spawn(handle(std::move(std::get<0>(result).value)));
        ++i;
    }
}

int main() {
    stdio_init_all();
    cyw43_arch_init();
    cyw43_arch_enable_sta_mode();
    cyw43_arch_wifi_connect_timeout_ms("SSID", "password",
                                       CYW43_AUTH_WPA2_AES_PSK, 15000);
    std::printf("IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));

    coro::Runtime rt;
    rt.block_on(run_server());

    cyw43_arch_deinit();
}
```

---

## Known limitations and future work

### Single active read and write per stream

`LwipTcpCtx` stores one `rx_waker` and one `tx_waker`. Calling `read()` from two
concurrent tasks on the same stream would silently overwrite the waker and lose
wakeups. Document and enforce single-consumer usage per direction; the same restriction
exists in the libuv `TcpStream`.

### `rx_buf` memory pressure

Incoming data is eagerly copied from pbufs into a `std::vector<uint8_t>`. On a busy
stream this allocation grows until `read()` drains it. For memory-constrained
applications consider capping `rx_buf` at a fixed size and pausing `tcp_recv` (by
returning `ERR_WOULDBLOCK`) when it fills.

### Timer resolution

`sleep_for` / `timeout` resolution is bounded by poll loop iteration time rather than
a hardware alarm. In practice the loop is tight (sub-millisecond), but the Pico SDK's
`add_alarm_in_us()` / hardware timer could provide sub-100µs resolution if needed.

### IPv6

`tcp_new()` creates an IPv4-only PCB. For dual-stack, replace with
`tcp_new_ip_type(IPADDR_TYPE_ANY)` and use `ip_addr_t` throughout. lwIP's DNS resolver
already returns an `ip_addr_t` that holds either address family.

### Potential future primitives

| Primitive | Notes |
|---|---|
| GPIO edge | `gpio_set_irq_enabled_with_callback` → `oneshot::send()` in ISR |
| DMA completion | `dma_channel_set_irq0_enabled` → `oneshot::send()` in DMA IRQ |
| UART async | PIO UART or SDK UART with RX IRQ → `mpsc::send()` |
| I2C / SPI async | DMA-backed transfers, completion via IRQ → `oneshot::send()` |
| ADC burst | DMA from ADC FIFO, completion via IRQ |
| TLS | mbedTLS layered over `TcpStream` |

The `oneshot` / `mpsc` channel pattern (sender called from ISR, receiver awaited in
coroutine) is the recommended integration point for all hardware peripherals. No
custom `Future` type is required.
