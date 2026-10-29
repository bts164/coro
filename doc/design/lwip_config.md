# lwIP configuration ownership

## Problem

lwIP's headers resolve `lwipopts.h` via an unqualified `#include "lwipopts.h"`
— whichever include directory is searched first that contains a matching
file wins. Historically every Pico application in this repo provided its
own copy (`examples/pico/lwipopts.h`, `examples/pico/epaper/lwipopts.h` —
byte-identical) and had to remember to add it to `coro_pico`'s include path.
Forgetting it is a build error; subtly diverging it is worse, since
`lwipopts.h` affects the ABI of every translation unit that includes lwIP
headers, including the ones inside `coro_pico` itself.

This is also a real Conan binary-compatibility hazard, not just a CMake
ergonomics one: if `coro_pico`'s package ID doesn't change when
`lwipopts.h` changes, two applications with different lwIP configs could be
served the *same cached binary* from Conan's cache — built against a
config that doesn't match either of them.

## Design

**`coro` owns `lwipopts.h` exclusively.** Any package that needs lwIP must
get it transitively through `coro` — by linking `coro::pico` (or
`coro::pico_mqtt`, etc.) — never by declaring its own copy or its own
include path for one. `coro_pico_mqtt` is the existing proof this works:
it needs `LWIP_MQTT 1`, but gets there through its `coro_pico` dependency,
not by bringing its own `lwipopts.h`. A future `coro_pico_https` (or any
other lwIP-touching module) must follow the same rule. This is a documented
constraint, not something enforced generically across an arbitrary
dependency graph — a second, independent lwIP-using Conan package that
insists on its own config is an accepted incompatibility, not a problem
this design tries to solve.

**Feature toggles are coro options, not CMake-variable plumbing.** Content
that genuinely varies by feature (currently just `LWIP_MQTT`) is controlled
by a `coro` Conan option (`with_mqtt`), which:

- Drives `#cmakedefine01 LWIP_MQTT` in `src/detail/sys/lwip/lwipopts.h.in`, expanded
  via `configure_file()` in `cmake/platforms/pico.cmake` into
  `${CMAKE_CURRENT_BINARY_DIR}/coro_pico_lwipopts/lwipopts.h` (mirrored by
  the plain CMake variable `CORO_PICO_WITH_MQTT` for non-Conan / source
  builds, e.g. `examples/pico/CMakeLists.txt`).
- Automatically participates in Conan's package ID — an app built with
  `with_mqtt=True` gets a distinctly cached `coro` binary from one built
  without it, with no extra wiring needed.
- Is read by `cmake/platforms/pico_mqtt.cmake`, which `FATAL_ERROR`s if
  included before `pico.cmake` (so it can't end up pointed at a different
  `lwipopts.h` than `coro_pico` itself was compiled against).

This was deliberately *not* built as a generic per-knob `configure_file()`
parameterization (e.g. a CMake variable for every `#define` in the file) —
that would be solving tuning needs nobody has hit yet. `LWIP_MQTT` earned
its own toggle because a real, existing consumer (`coro_pico_mqtt`) needs
it; nothing else has.

**The full-file override remains for genuinely custom tuning.** Setting
`CORO_PICO_LWIPOPTS_DIR` to a directory containing a complete, hand-written
`lwipopts.h` bypasses generation entirely — for tuning beyond what coro's
own options cover (e.g. non-default `MEM_SIZE`/`TCP_WND` for a
memory-constrained board). `pico.cmake` and `pico_mqtt.cmake` both consume
the same resolved directory (`CORO_PICO_LWIPOPTS_INCLUDE_DIR`), so an
override always applies consistently across every coro-provided lwIP
target in one build — never just one of them.

## Loopback

The bundled `lwipopts.h` enables lwIP's loopback interface (`LWIP_HAVE_LOOPIF`) and
loopback on every interface (`LWIP_NETIF_LOOPBACK`). A connection to `127.0.0.1`, or
to the device's own address, reaches the device itself, as it does on a desktop.

lwIP does not deliver a looped packet when it is sent. In a build without an operating
system it puts the packet on a queue and waits for `netif_poll_all()` to be called.
Neither the Pico SDK nor the CYW43 driver calls it, so the lwIP `Poller` does, every time
it polls, until the queue is empty. The queue is not an `async_context` wake source, so
the `Poller` never sleeps while it holds a packet (see [Pico Port](pico_port.md#poller)).

How lwIP is run is chosen when the runtime is constructed, with `coro::PicoNetwork`:

| `PicoNetwork` | lwIP runs on | The application must first call |
|---|---|---|
| `Cyw43` (default) | The CYW43 driver's `async_context`, set up by `cyw43_arch_init()` | `cyw43_arch_init()` |
| `Lwip` | The `Runtime`'s own `async_context`, set up by the `Runtime` with `lwip_nosys_init()` | nothing |
| `None` | Nothing: lwIP is not initialised | nothing |

`Lwip` is lwIP without the radio. Loopback is the only interface unless the application
adds its own, and it works on a plain Pico. The on-target networking tests use it.

!!! note "NOTE: a `PicoNetwork::None` runtime has no sockets"
    That runtime does not initialise or run lwIP. Creating a socket or resolving a name
    on it throws `std::system_error` with `ENETDOWN` at the `co_await`, before lwIP is
    touched (see [Pico Port](pico_port.md#runtime-on-pico)).

!!! danger "WARNING: the default runtime lives between `cyw43_arch_init()` and `cyw43_arch_deinit()`"
    It runs on the CYW43 driver's `async_context`, which only `cyw43_arch_init()`
    creates and `cyw43_arch_deinit()` destroys. Construct it after the first and
    destroy it before the second. Firmware that does not start the radio must construct
    the runtime with `PicoNetwork::Lwip` or `PicoNetwork::None`.

!!! note "NOTE: the loopback queue has no length limit"
    `LWIP_LOOPBACK_MAX_PBUFS` is left at lwIP's default of 0, which means unlimited.
    The queue is emptied on every poll, so it holds only what tasks send to the device
    itself between two driver turns. Each queued packet is a copy allocated from the
    heap.

## Receive memory

The lwIP `sys` backend copies received data out of each pbuf into a buffer owned by the
socket and frees the pbuf in the same callback (see
[Pico Port](pico_port.md#received-data-per-socket-buffers)). The two kinds of memory
involved are sized separately:

- **`PBUF_POOL`** is what the CYW43 driver receives every frame into. Pool buffers are
  held only while a callback runs, never while data waits for a reader, so
  `PBUF_POOL_SIZE` covers frames in flight and nothing else. Unread data on one socket
  cannot starve the radio for the others.
- **Socket buffers** come from the heap, and each has a fixed bound:

```
heap for received data  <=  (TCP connections with data × TCP_WND)
                          + (UDP sockets × CORO_LWIP_UDP_RX_BYTES)
```

A TCP connection's ring is `TCP_WND` bytes, allocated when its first data arrives. It
can never overflow: lwIP's receive window, which the application reopens only by reading
(`tcp_recved()`), stops the peer once `TCP_WND` bytes are unread. A UDP socket's ring is
`CORO_LWIP_UDP_RX_BYTES` (default 8192), allocated when it is bound; a datagram that
does not fit is dropped.

With the bundled `TCP_WND` of 8 × 1460 bytes, each TCP connection that has received data
costs about 11.4 KB of heap. `TCP_WND` therefore trades per-connection throughput
against RAM. The pool size only needs to change if the radio itself drops frames under
load.

## Settings the `sys` backend requires

| Setting | Value | Why |
|---|---|---|
| `NO_SYS` | 1 | The raw API, driven from the executor's thread |
| `LWIP_TCP`, `LWIP_UDP`, `LWIP_DNS`, `LWIP_IGMP` | 1 | The socket types, `lookup_host()` and multicast |
| `SO_REUSE` | 1 | `SOF_REUSEADDR` on listeners, matching `SO_REUSEADDR` on desktop |
| `TCP_LISTEN_BACKLOG` | 1 | `tcp_backlog_delayed()`/`tcp_backlog_accepted()`, so lwIP enforces the listener's backlog until `accept()` |
| `LWIP_TIMERS` | 1 | lwIP's timers run as an at-time worker on the `async_context` (`pico_lwip_nosys`), which also bounds the CPU's sleep |
| `LWIP_NETIF_LOOPBACK` | 1 | The `Poller` checks each netif's `loop_first` to drain the loopback queue before it sleeps |

A custom `lwipopts.h` must keep these.

## Future: packages split out of `coro` itself

If `coro_pico_mqtt` (or a future `coro_pico_https`) is ever split into its
*own* Conan package rather than living inside `coro`'s own CMakeLists.txt,
the options-as-package-ID mechanism alone isn't enough — Conan doesn't
stop two independently-versioned recipes from disagreeing about a shared
option's value. The pattern for that case is `validate()`:

```python
def validate(self):
    if not self.dependencies["coro"].options.with_mqtt:
        raise ConanInvalidConfiguration(
            "coro_pico_mqtt requires coro built with with_mqtt=True")
```

This raises a hard, early error rather than a silent link-against-the-wrong-
config failure. Not implemented yet because there is no split-out package
yet — `coro_pico_mqtt` still lives in `coro`'s own `cmake/platforms/`. Add
this `validate()` the day a `coro_pico_mqtt` *package* (separate
`conanfile.py`) is introduced.
