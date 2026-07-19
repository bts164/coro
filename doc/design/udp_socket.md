# UDP Socket

`UdpSocket` — async, connectionless UDP send/recv. Mirrors `TcpStream`/`TcpListener`'s
dual-backend structure: a libuv implementation for desktop builds, and an lwIP
implementation for the Pico port (`CORO_PICO`).

---

## Overview

`UdpSocket` exposes a `sendto`/`recvfrom`-style API — every send specifies a destination,
every receive reports who it came from. It also supports an optional `connect()`-fixed-peer
mode, mirroring BSD "connected" UDP sockets: once connected, `send()`/`recv()` drop the
address argument and the OS (or lwIP) filters out datagrams from any other peer.

```cpp
UdpSocket sock = co_await UdpSocket::bind("0.0.0.0", 9000);

// Send a datagram to a specific peer. SocketAddress::parse validates the
// address up front rather than deferring failure to send_to().
auto peer = SocketAddress::parse("127.0.0.1", 9001).value();
co_await sock.send_to(std::string("hello"), peer);

// Receive the next datagram into the caller's buffer; reports who sent it.
auto [n, buf, sender] = co_await sock.recv_from(std::string(1500, '\0'));
buf.resize(n);
std::printf("got %zu bytes from %s\n", n, sender.to_string().c_str());

// Or fix a single peer up front and drop the address argument on every call:
UdpSocket client = co_await UdpSocket::bind("0.0.0.0", 0);
co_await client.connect(peer);
co_await client.send(std::string("hello"));
auto [n2, buf2] = co_await client.recv(std::string(1500, '\0'));
```

Both directions take the caller's buffer by value and return it — no span or raw pointer
ever escapes the I/O operation (see [`ByteBuffer`](../../include/coro/io/byte_buffer.h)).
Unlike `send_to`/`send`, `recv_from`/`recv` can't know the datagram's size ahead of time,
so oversized datagrams are truncated to fit the caller's buffer, same as POSIX
`recvfrom()`. There is no internal receive queue — `coro` does not buffer datagrams in
userspace on either backend. On the desktop (libuv) backend, `recv_from`/`recv` first
attempt a direct, non-blocking read on the calling thread; if nothing is available yet,
the call suspends and the kernel's own per-socket receive buffer holds any datagrams that
arrive in the meantime, exactly as it would for any other UDP socket — see
[Receive path](#receive-path) below. lwIP has no equivalent OS-level buffer, so a datagram
that arrives while nothing is awaiting `recv_from`/`recv` on the Pico backend is dropped;
see the same section for why.

---

## `SocketAddress` — new shared address type

Nothing in the codebase today needs to report a peer address back to the caller —
`TcpStream::connect(host, port)` and `TcpListener::bind(host, port)` only ever take an
address as input. `recv_from()` is the first API that needs to *produce* one, so this
design introduces the library's first address type.

Modeled on Rust's `SocketAddr`/`Ipv4Addr`/`Ipv6Addr` split, but spelled out in full
(`SocketAddress`) to match this codebase's existing naming (`JoinHandle`,
`CancellationToken`) rather than Rust's abbreviation. Like Rust's version, addresses are
stored as fixed-size byte arrays, never as strings:

```cpp
// include/coro/io/socket_address.h
namespace coro {

struct Ipv4Address {
    std::array<uint8_t, 4> octets{};
};

struct Ipv6Address {
    std::array<uint8_t, 16> octets{};
    // Interface index disambiguating link-local addresses (fe80::/10), which are not
    // globally unique — the same address can be valid on multiple interfaces at once.
    // Zero for global-scope addresses, where it's meaningless. Corresponds directly to
    // sockaddr_in6::sin6_scope_id.
    uint32_t scope_id = 0;
};

struct SocketAddress {
    std::variant<Ipv4Address, Ipv6Address> address;
    uint16_t port = 0;

    /// Parses a numeric IPv4 or IPv6 address ("127.0.0.1", "::1", "fe80::1%3") plus
    /// port into a SocketAddress. Returns std::nullopt on malformed input — there is
    /// no way to construct a SocketAddress holding an invalid address.
    static std::optional<SocketAddress> parse(std::string_view host, uint16_t port);

    /// Renders back to text, e.g. "127.0.0.1:9001" or "[fe80::1%3]:9001". For
    /// logging/diagnostics only — allocates, so it must never sit on the send/recv
    /// hot path.
    std::string to_string() const;
};

} // namespace coro
```

`Ipv4Address` and `Ipv6Address` are trivially-copyable, fixed-size value types (5 and 20
bytes respectively) — no heap allocation, which matters as much for the Pico port as for
avoiding an unnecessary allocation on every desktop `send_to`/`recv_from`. Validation
happens once, at `parse()`, rather than being deferred to whatever eventually calls
`uv_ip4_addr`/`inet_pton` on a stored string. `parse()` is implemented with `inet_pton()`
on the libuv backend and `ip4addr_aton`/`ip6addr_aton` on the lwIP backend.

Shared by both backends and placed alongside `byte_buffer.h` under `include/coro/io/`
since, like `ByteBuffer`, it's a plain value type with no backend-specific code.
`Ipv6Address` exists in the type from day one, but see [Known limitations](#known-limitations--future-work) —
the Pico/lwIP backend only supports IPv4 in this design, consistent with the existing
`pico_port.md` limitation for `TcpStream`/`TcpListener`.

---

## Receive path

Every earlier draft of this section assumed `coro` needed to buffer arriving datagrams
itself, in userspace, between the libuv/lwIP callback and whenever the caller next awaits
`recv_from()`/`recv()` — first via a standalone `BufferPool`/`PooledBuffer` abstraction,
then via a `PollStream`-style internal `RingBuffer<DatagramEntry>` queue with a
`BackpressureMode` policy. Both were dropped: on the desktop (libuv) backend, the kernel's
own per-socket receive buffer already does exactly this job "for free," the same way it
does for any other UDP socket in any other language — duplicating it in userspace only
added bookkeeping (queue capacity, drop policy) without adding any real capability, since
the one thing our own queue could do differently from the kernel — drop the *oldest*
queued datagram instead of the newest when full — isn't something this design needs to
provide. So `coro` does not buffer datagrams itself at all. Instead:

- **Desktop (libuv):** `recv_from()`/`recv()` first attempt a direct, non-blocking
  `recvfrom()` on the raw socket fd, on whichever thread called them — no uv-thread hop.
  If a datagram is already sitting in the kernel's receive buffer, this returns
  immediately with zero suspension and zero extra copies. Only if that comes up empty
  does the call suspend, arming `uv_udp_recv_start()` for exactly this one call and
  handing libuv the caller's own buffer directly (no scratch buffer, no intermediate
  copy) — the kernel buffers everything that arrives in between, same as it always does.
- **Pico (lwIP):** there is no raw-fd fast path (lwIP has no socket fd to read from
  directly, and no OS-level receive buffer underneath it at all) and no internal queue
  either, so `recv_from()`/`recv()` always suspend, registering `udp_recv()` for exactly
  this one call. A datagram that arrives while nothing is registered — i.e. while no
  `recv_from()`/`recv()` call is currently awaiting — is simply dropped by lwIP itself,
  with no buffer anywhere to catch it. This is a real, backend-specific behavior
  difference from the desktop backend, called out again in
  [Known limitations](#known-limitations--future-work).

### libuv: raw-fd fast path, arm-and-wait inside a single `with_context` hop

`Handle` caches the raw fd once (`uv_fileno()`, in `bind()`) — that's the only thing it
holds. There's no per-call state on `Handle` at all: the slow path's arm/wait both happen
*inside* the one `with_context` coroutine that runs on the uv thread, using the exact
same `UvCallbackResult`/`wait()` bridge `TcpStream::connect()` and `~TcpStream()` already
use (`include/coro/runtime/uv_future.h`) — no bespoke synchronization primitive needed:

```cpp
struct Handle {
    uv_udp_t handle;
    int      raw_fd = -1;   // cached via uv_fileno() in bind(); lets recv_from()
                            // attempt a raw read without a uv-thread hop — see below
};
```

`recv_from_impl` tries the fast path first; only on `EAGAIN` does it hop to the uv thread,
where a small per-call `RecvCtx` (stack-local to that hop, not stored on `Handle`) carries
the caller's buffer pointer to `alloc_cb` and the `UvCallbackResult` to `recv_cb`:

```cpp
template<ByteBuffer Buf>
Coro<std::tuple<std::size_t, Buf, SocketAddress>> recv_from_impl(
        std::shared_ptr<Handle> handle, SingleThreadedUvExecutor* uv_exec, Buf buf) {
    // Fast path: try a non-blocking recvfrom() directly, on whichever thread called us.
    // Safe unconditionally — see "Fast path safety" below.
    sockaddr_storage storage;
    socklen_t addrlen = sizeof(storage);
    ssize_t n = ::recvfrom(handle->raw_fd, buf.data(), buf.size(), MSG_DONTWAIT,
                           reinterpret_cast<sockaddr*>(&storage), &addrlen);
    if (n >= 0) {
        co_return {static_cast<std::size_t>(n), std::move(buf),
                   detail::from_sockaddr(reinterpret_cast<sockaddr*>(&storage))};
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
        throw_errno_error(errno, "UdpSocket::recv_from");

    // Slow path: genuinely nothing available yet. Hop to the uv thread; arming and
    // waiting both happen inside this one with_context coroutine, so recv_cb firing
    // is what resumes it directly — no separate waker/ready flag to manage.
    auto [nread, sender] = co_await with_context(*uv_exec,
        [](std::shared_ptr<Handle> handle, std::byte* buf, std::size_t len)
                -> Coro<std::tuple<ssize_t, SocketAddress>> {
            UvCallbackResult<ssize_t, SocketAddress> result;
            struct RecvCtx {
                std::byte*                            buf;
                std::size_t                           len;
                UvCallbackResult<ssize_t, SocketAddress>* result;
            } ctx{buf, len, &result};
            handle->handle.data = &ctx;

            uv_udp_recv_start(&handle->handle,
                [](uv_handle_t* h, std::size_t, uv_buf_t* out) {
                    auto* ctx = static_cast<RecvCtx*>(h->data);
                    // No scratch buffer: libuv reads straight into the caller's own
                    // buffer. Safe because the coroutine owning it is suspended
                    // (pinned in memory) for the entire duration of this call.
                    out->base = reinterpret_cast<char*>(ctx->buf);
                    out->len  = static_cast<unsigned>(ctx->len);
                },
                [](uv_udp_t* h, ssize_t nread, const uv_buf_t*,
                   const struct sockaddr* addr, unsigned /*flags*/) {
                    if (nread == 0 && addr == nullptr) return; // no more data this tick

                    uv_udp_recv_stop(h); // single-shot: exactly one datagram per armed call
                    auto* ctx = static_cast<RecvCtx*>(reinterpret_cast<uv_handle_t*>(h)->data);
                    SocketAddress sender = nread >= 0 ? detail::from_sockaddr(addr) : SocketAddress{};
                    ctx->result->complete(nread, sender);
                });

            co_return co_await wait(result);
        }(handle, reinterpret_cast<std::byte*>(buf.data()), buf.size())
    );

    if (nread < 0) throw_uv_error(static_cast<int>(nread), "UdpSocket::recv_from");
    co_return {static_cast<std::size_t>(nread), std::move(buf), sender};
}
```

`recv_from_impl` itself is a plain `Coro`, not wrapped in `with_context` — the fast path
never touches the uv thread at all, and the slow path hops there exactly once, awaited
directly (not fire-and-forget: the caller needs the result). `UdpSocket::recv_from()`/
`recv()` no longer force a uv-thread round trip on every call, unlike `send_to()`/
`send()`/`connect()` (which still call real libuv functions and so still need
`with_context`).

### Fast path safety: mutually exclusive by construction

Could the fast-path `recvfrom()` above race with `recv_cb`, which also calls `recvfrom()`
under the hood via libuv? No — and unlike an earlier draft where `uv_udp_recv_start`
stayed continuously armed (requiring an argument about the kernel's receive queue being
FIFO), there isn't even shared mutable state to reason about here: `Handle` holds nothing
but the immutable `raw_fd`, and `RecvCtx` is a stack-local temporary that only exists for
the duration of one armed `with_context` hop. Combined with the existing restriction that
only one `recv_from()`/`recv()` call may be in flight at a time:

- The fast path only ever runs at the very start of a call, before that call has armed
  anything.
- By the time a call reaches its fast-path attempt, any *previous* call has already fully
  disarmed (its slow path, if it took one, already completed — its `with_context` hop
  can't return until `recv_cb` has called `uv_udp_recv_stop` and completed the result).

So the raw fast-path read and `recv_cb`'s own read are never live at the same time for the
same socket — full stop, no kernel-semantics argument required. This also means the
concern doesn't even arise for `TcpStream` in quite the shape described in an earlier
draft of this note; see the TODO in [Known limitations](#known-limitations--future-work)
for what would still need re-deriving there.

!!! note "NOTE: the close race is a pre-existing hazard, not a new one"
    Could `handle->raw_fd` be closed by a concurrent `uv_close()` between fetching it and
    calling `recvfrom()`? `UdpSocket` is the RAII owner of `Handle`, and `recv_from()` is a
    member call — the caller's own instance (or its `shared_ptr`) already keeps the
    `Handle` alive for the duration of the call, so the socket cannot be destroyed by the
    same call chain. The only way to close concurrently is an explicit `close()`/destructor
    call from a *different* coroutine while a `recv_from()` is in flight — already the same
    class of restriction as any other concurrent operation on one `UdpSocket`, not something
    this optimization introduces. And even libuv itself is not immune to the single-threaded
    version of this race: because the uv thread only ever runs one callback at a time,
    scheduling a close *before* a pending `recv_cb` fires produces the same "handle closes
    out from under an in-flight read" ordering today, with or without this fast path. Worst
    case if the raw fd is closed mid-`recvfrom()` is an ordinary `EBADF` error return, not
    memory corruption.

---

## Public API

```cpp
// include/coro/io/udp_socket.h
class UdpSocket {
public:
    UdpSocket(UdpSocket&&) noexcept;
    UdpSocket& operator=(UdpSocket&&) noexcept;
    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    ~UdpSocket();

    /// Binds a UDP socket to host:port. host must be dotted-decimal, an IPv6
    /// literal, or "0.0.0.0"/"::".
    [[nodiscard]] static /* Future<UdpSocket> */ bind(std::string host, uint16_t port);

    /// Sends buf as a single datagram to dest. Returns buf once the send completes.
    template<ByteBuffer Buf>
    [[nodiscard]] /* Future<Buf> */ send_to(Buf buf, SocketAddress dest);

    /// Waits for the next datagram, copying it into buf. Returns {n, buf, sender},
    /// where n is the number of bytes written into buf. If the datagram was larger
    /// than buf, it is truncated to fit — same as POSIX recvfrom(). Suspends if none
    /// has arrived yet; see [Receive path](#receive-path) for the fast-path-then-suspend
    /// model — there is no internal queue, so datagrams that arrive while nothing is
    /// awaiting are handled entirely by the kernel (libuv backend) or dropped (lwIP
    /// backend, which has no equivalent buffer).
    template<ByteBuffer Buf>
    [[nodiscard]] /* Future<std::tuple<std::size_t, Buf, SocketAddress>> */ recv_from(Buf buf);

    /// Fixes peer as this socket's only correspondent. Once connected, plain
    /// send()/recv() may be used instead of send_to()/recv_from(); datagrams from
    /// any other address are dropped by the OS (libuv backend) or by pcb->remote_ip
    /// filtering (lwIP backend) before they ever reach recv().
    [[nodiscard]] /* Future<void> */ connect(SocketAddress peer);

    /// Sends buf to the peer fixed by connect(). Throws if not connected.
    template<ByteBuffer Buf>
    [[nodiscard]] /* Future<Buf> */ send(Buf buf);

    /// Waits for the next datagram from the peer fixed by connect(), copying it
    /// into buf. Throws if not connected. Same truncation contract as recv_from().
    template<ByteBuffer Buf>
    [[nodiscard]] /* Future<std::tuple<std::size_t, Buf>> */ recv(Buf buf);

    /// Enables (or disables) sending to broadcast addresses via send_to()/send().
    /// Required on the libuv backend before a sendto() to a broadcast address is
    /// permitted by the OS (SO_BROADCAST) — otherwise it fails with EACCES. A no-op
    /// on the lwIP backend, which doesn't gate broadcast on this build's config; see
    /// [Multicast and broadcast](#multicast-and-broadcast).
    [[nodiscard]] /* Future<void> */ set_broadcast(bool enabled);

    /// Joins multicast group so recv_from()/recv() start receiving datagrams sent to
    /// it. iface selects which local interface to join on; the default
    /// (Ipv4Address{}) lets the OS (libuv) or the single Pico interface (lwIP) choose.
    /// IPv4 only — see [Multicast and broadcast](#multicast-and-broadcast).
    [[nodiscard]] /* Future<void> */ join_multicast(Ipv4Address group, Ipv4Address iface = {});

    /// Leaves a multicast group previously joined with join_multicast().
    [[nodiscard]] /* Future<void> */ leave_multicast(Ipv4Address group, Ipv4Address iface = {});
};
```

The exact return type (`Coro<T>` vs `JoinHandle<T>`) follows `TcpStream`'s existing
per-backend split, with one exception carved out for the fast path described in
[Receive path](#receive-path):

| Backend | Method | Return type | Why |
|---|---|---|---|
| libuv (desktop) | `send_to()`, `send()`, `connect()` | `JoinHandle<T>` | Calls a real libuv function, so it still runs via `with_context(uv_exec, ...)` — same as every `TcpStream`/`TcpListener` method today |
| libuv (desktop) | `recv_from()`, `recv()` | `Coro<T>` | The raw-fd fast-path read runs directly on the caller's thread — no uv-thread hop needed except on the (rare) slow path, which does need one `with_context` hop (awaited, since the result comes from there), unlike `PollStream`'s always-armed model |
| libuv (desktop) | `set_broadcast()`, `join_multicast()`, `leave_multicast()` | `JoinHandle<void>` | `uv_udp_set_broadcast()`/`uv_udp_set_membership()` are libuv calls on the handle, so — same as `connect()` — they go via `with_context(uv_exec, ...)` even though neither one itself suspends |
| lwIP (Pico) | all | `Coro<T>` | Callbacks fire synchronously inside the caller's own executor tick — no thread hop, so a plain `Coro` suffices, same as `TcpStream`'s lwIP methods |

**Concurrency** (matches the existing `TcpStream` restriction): only one receive
(`recv_from()`/`recv()`) may be in flight at a time, and only one send (`send_to()`/`send()`)
may be in flight at a time. A concurrent receive + send pair is fine — they use
independent wakers. `connect()` itself does not suspend on either backend (see below) and
must not be called concurrently with a send or receive already in flight, since it
mutates the same `Handle`/`LwipUdpCtx` those operations read.

**Mixing `_to`/`_from` calls with `connect()`:** once `connect()` has been called,
`recv_from()` remains callable on both backends, but `send_to()` does not — the two
backends disagree on whether an explicit destination is even accepted:

!!! warning "WARNING: `send_to()` always throws EISCONN after connect() on the libuv/Linux backend"
    On Linux, once a UDP socket has been `connect()`-ed, the kernel rejects
    *any* `sendto()` that specifies a destination address with `EISCONN`
    ("Transport endpoint is already connected") — this holds even if the address
    given is exactly the address passed to `connect()`. Per `sendto(2)`: "the
    connection-mode socket was connected already but a recipient was specified" —
    Linux does not special-case a recipient that happens to match the connected
    peer. In practice this means `send_to()` is unusable after `connect()` on this
    backend; callers must switch to `send()` once connected. `recv_from()` is
    unaffected: the kernel only ever delivers datagrams from the connected peer
    once `connect()` has run, so `recv_from()` and `recv()` behave identically on
    this backend.

    The lwIP backend has no such restriction — `udp_sendto()` always accepts an
    explicit destination regardless of connected state, since lwIP never filters
    sends by the connected peer the way the Linux kernel does. Portable code that
    needs `send_to()` to keep working after `connect()` should not rely on this
    across backends; see [Known limitations](#known-limitations--future-work).

---

## Multicast and broadcast

Both are supported in this first iteration rather than deferred, since the underlying
mechanism already exists on both backends:

- **Multicast (libuv):** `uv_udp_set_membership()` joins/leaves an IGMP group on a raw
  socket — no new dependency.
- **Multicast (lwIP):** `LWIP_IGMP` is already `1` in this project's bundled
  `lwipopts.h.in` (enabled for the mDNS responder's own group join), so
  `igmp_joingroup_netif()`/`igmp_leavegroup_netif()` are already compiled into every Pico
  build — nothing to newly enable, just to call.
- **Broadcast (libuv):** the OS refuses a `sendto()` to a broadcast address with `EACCES`
  unless `SO_BROADCAST` is set first (`uv_udp_set_broadcast()`), so `set_broadcast(true)`
  must be called explicitly before broadcasting.
- **Broadcast (lwIP):** no explicit enable step is needed. lwIP's raw UDP API only
  enforces an `SO_BROADCAST`-equivalent check (an `SOF_BROADCAST` flag on the pcb) when
  `IP_SOF_BROADCAST`/`IP_SOF_BROADCAST_RECV` are defined; both default to `0` in lwIP's
  `opt.h` and are left unset in this project's `lwipopts.h.in`, so the check in
  `udp_sendto_if()` (`src/core/udp.c`) is compiled out entirely and `udp_sendto()` to a
  broadcast address just works. `set_broadcast()` is a no-op on this backend, kept only
  for API symmetry — see [Pico (lwIP) backend](#pico-lwip-backend) below.

**Scope kept deliberately narrow for this iteration:** `group`/`iface` are `Ipv4Address`,
not `SocketAddress` — there is no IPv6 multicast (MLD) support, consistent with the
existing IPv4-only limitation for the lwIP backend generally (see [Known
limitations](#known-limitations--future-work)). On the Pico backend, `iface` is accepted
for API symmetry with the desktop backend but ignored — a Pico target has exactly one
network interface, so there's nothing to select between; `join_multicast`/
`leave_multicast` always act on `netif_default`. Source-specific multicast (IGMPv3),
multicast TTL/loopback control, and directed (non-limited) broadcast address computation
from a subnet mask are all left for a future iteration.

---

## Backend flag

Per the project's existing (deferred) [[backend flag scheme]] design question, UDP gets
its own flag rather than reusing `CORO_TCP_BACKEND_LWIP`, since the two are logically
independent components that happen to be enabled together today:

| Flag | Effect |
|---|---|
| `CORO_UDP_BACKEND_LWIP` | Selects the lwIP backend for `UdpSocket` |

`cmake/platforms/pico.cmake` defines both `CORO_TCP_BACKEND_LWIP` and
`CORO_UDP_BACKEND_LWIP` for real Pico builds, same as it does today for TCP alone. A
desktop build defines neither, so `udp_socket.h`'s `#else` branch (libuv) is compiled.

---

## Desktop (libuv) backend

### `SocketAddress` ⇄ `sockaddr` conversion

Both `send_to`/`recv_from` and `connect`/`send`/`recv` need to cross between
`SocketAddress` and libuv's `sockaddr`/`sockaddr_storage`. Two small internal helpers
(not part of the public API) handle both directions, dispatching on the `SocketAddress`
variant / `sockaddr::sa_family` respectively. These live under `include/coro/io/` rather
than `src/io/` — `udp_socket.hpp`'s template method bodies need them, and `udp_socket.hpp`
is itself a public header included from `udp_socket.h`:

```cpp
// include/coro/io/socket_address_uv.h
namespace coro::detail {

socklen_t to_sockaddr(const SocketAddress& addr, sockaddr_storage& out) {
    return std::visit(overloaded{
        [&](const Ipv4Address& v4) -> socklen_t {
            auto* sin = reinterpret_cast<sockaddr_in*>(&out);
            sin->sin_family = AF_INET;
            sin->sin_port   = htons(addr.port);
            std::memcpy(&sin->sin_addr, v4.octets.data(), 4);
            return sizeof(sockaddr_in);
        },
        [&](const Ipv6Address& v6) -> socklen_t {
            auto* sin6 = reinterpret_cast<sockaddr_in6*>(&out);
            sin6->sin6_family   = AF_INET6;
            sin6->sin6_port     = htons(addr.port);
            sin6->sin6_scope_id = v6.scope_id;
            std::memcpy(&sin6->sin6_addr, v6.octets.data(), 16);
            return sizeof(sockaddr_in6);
        },
    }, addr.address);
}

SocketAddress from_sockaddr(const sockaddr* addr) {
    if (addr->sa_family == AF_INET) {
        auto* sin = reinterpret_cast<const sockaddr_in*>(addr);
        Ipv4Address v4;
        std::memcpy(v4.octets.data(), &sin->sin_addr, 4);
        return SocketAddress{v4, ntohs(sin->sin_port)};
    }
    auto* sin6 = reinterpret_cast<const sockaddr_in6*>(addr);
    Ipv6Address v6;
    std::memcpy(v6.octets.data(), &sin6->sin6_addr, 16);
    v6.scope_id = sin6->sin6_scope_id;
    return SocketAddress{v6, ntohs(sin6->sin6_port)};
}

} // namespace coro::detail
```

### `send_to` — single-shot, like `TcpStream::write`

`uv_udp_send()` is asynchronous and takes a destination `sockaddr` directly — no
separate connect step needed:

```cpp
template<ByteBuffer Buf>
JoinHandle<Buf> UdpSocket::send_to(Buf buf, SocketAddress dest) {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle, Buf buf, SocketAddress dest) -> Coro<Buf> {
            auto view = std::as_bytes(std::span(buf));

            sockaddr_storage storage;
            socklen_t addrlen = detail::to_sockaddr(dest, storage);

            UvCallbackResult<int> result;
            uv_udp_send_t req;
            uv_buf_t bdesc = uv_buf_init(
                const_cast<char*>(reinterpret_cast<const char*>(view.data())),
                static_cast<unsigned>(view.size()));
            req.data = &result;

            int r = uv_udp_send(&req, &handle->handle, &bdesc, 1,
                reinterpret_cast<const struct sockaddr*>(&storage),
                [](uv_udp_send_t* r, int status) {
                    static_cast<UvCallbackResult<int>*>(r->data)->complete(status);
                });
            if (r < 0) throw_uv_error(r, "UdpSocket::send_to");

            auto [status] = co_await wait(result);
            if (status != 0) throw_uv_error(status, "UdpSocket::send_to");
            co_return std::move(buf);
        }(m_handle, std::move(buf), dest)
    );
}
```

### `recv_from` — raw-fd fast path, per-call arm/disarm

Unlike `PollStream`'s always-armed model, `uv_udp_recv_start()` here is armed only for
the duration of one suspended `recv_from()`/`recv()` call, matching `TcpStream`'s
per-call arm/disarm shape rather than `PollStream`'s. Most calls never arm anything at
all: the raw-fd fast path attempts a non-blocking `recvfrom()` directly, and only falls
back to arming when that comes up empty. The `Handle` and `recv_from_impl()` shown here
are exactly the ones introduced in [Receive path](#receive-path) above — this section
just places them in context alongside `bind()`/`send_to()`/`connect()`.

### `connect`, `send`, `recv` — fixed-peer mode

`uv_udp_connect()` is synchronous and just stores the peer's `sockaddr` on the kernel
socket — the OS then filters incoming datagrams to that peer for free. `send()`/`recv()`
are `send_to()`/`recv_from()` with the address argument dropped: `send()` passes
`nullptr` as the destination to `uv_udp_send()` (libuv sends to the connected peer in
that case), and `recv()` reuses the exact same `recv_cb` as `recv_from()` — the sender
address is still reported by the callback but simply discarded, since libuv doesn't
distinguish "connected" sockets in `uv_udp_recv_start`'s callback signature:

```cpp
JoinHandle<void> UdpSocket::connect(SocketAddress peer) {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle, SocketAddress peer) -> Coro<void> {
            sockaddr_storage storage;
            socklen_t addrlen = detail::to_sockaddr(peer, storage);
            int r = uv_udp_connect(&handle->handle,
                reinterpret_cast<const struct sockaddr*>(&storage));
            if (r < 0) throw_uv_error(r, "UdpSocket::connect");
            co_return;
        }(m_handle, peer)
    );
}

template<ByteBuffer Buf>
JoinHandle<Buf> UdpSocket::send(Buf buf) {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle, Buf buf) -> Coro<Buf> {
            auto view = std::as_bytes(std::span(buf));

            UvCallbackResult<int> result;
            uv_udp_send_t req;
            uv_buf_t bdesc = uv_buf_init(
                const_cast<char*>(reinterpret_cast<const char*>(view.data())),
                static_cast<unsigned>(view.size()));
            req.data = &result;

            // dest == nullptr requires the handle to already be uv_udp_connect()-ed;
            // libuv returns UV_EDESTADDRREQ otherwise, surfaced as throw_uv_error below.
            int r = uv_udp_send(&req, &handle->handle, &bdesc, 1, nullptr,
                [](uv_udp_send_t* r, int status) {
                    static_cast<UvCallbackResult<int>*>(r->data)->complete(status);
                });
            if (r < 0) throw_uv_error(r, "UdpSocket::send");

            auto [status] = co_await wait(result);
            if (status != 0) throw_uv_error(status, "UdpSocket::send");
            co_return std::move(buf);
        }(m_handle, std::move(buf))
    );
}
```

`recv()` is `recv_from()` with the `SocketAddress` element of the returned tuple
dropped — implemented in terms of it rather than duplicated. It goes through the exact
same `recv_from_impl` machinery; the "connected" restriction is enforced by the OS
(non-peer datagrams never reach `recv_cb` in the first place), not by anything `recv()`
itself checks. Like `recv_from()`, it's a plain `Coro` — no `with_context` wrapper —
since `recv_from_impl` only hops to the uv thread on its slow path:

```cpp
template<ByteBuffer Buf>
Coro<std::tuple<std::size_t, Buf>> UdpSocket::recv(Buf buf) {
    auto [n, out, sender] = co_await recv_from_impl(m_handle, m_uv_exec, std::move(buf));
    co_return {n, std::move(out)};
}
```

### `set_broadcast`, `join_multicast`, `leave_multicast`

All three are synchronous libuv calls on the handle — like `connect()`, they still hop
via `with_context(uv_exec, ...)` since only the uv thread may touch `handle->handle`, but
none of them suspend once there. `join_multicast`/`leave_multicast` share one helper,
parameterized on `uv_membership`, converting the `Ipv4Address` octets to dotted-decimal
strings via `uv_inet_ntop()` since that's the form `uv_udp_set_membership()` takes. The
helper is a private static member of `UdpSocket` (not a `coro::detail` free function) —
`Handle` is a private nested type, so a free function outside the class can't name it:

```cpp
JoinHandle<void> UdpSocket::set_broadcast(bool enabled) {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle, bool enabled) -> Coro<void> {
            int r = uv_udp_set_broadcast(&handle->handle, enabled ? 1 : 0);
            if (r < 0) throw_uv_error(r, "UdpSocket::set_broadcast");
            co_return;
        }(m_handle, enabled)
    );
}

// private static member — declared in udp_socket.h alongside recv_from_impl
JoinHandle<void> UdpSocket::set_membership(std::shared_ptr<Handle> handle,
        SingleThreadedUvExecutor* uv_exec, Ipv4Address group, Ipv4Address iface,
        uv_membership membership) {
    return with_context(*uv_exec,
        [](std::shared_ptr<Handle> handle, Ipv4Address group, Ipv4Address iface,
           uv_membership membership) -> Coro<void> {
            char group_str[16];
            uv_inet_ntop(AF_INET, group.octets.data(), group_str, sizeof(group_str));

            char iface_str[16];
            bool has_iface = iface.octets != Ipv4Address{}.octets;
            if (has_iface) uv_inet_ntop(AF_INET, iface.octets.data(), iface_str, sizeof(iface_str));

            int r = uv_udp_set_membership(&handle->handle, group_str,
                has_iface ? iface_str : nullptr, membership);
            if (r < 0) throw_uv_error(r, "UdpSocket::join_multicast/leave_multicast");
            co_return;
        }(handle, group, iface, membership)
    );
}

JoinHandle<void> UdpSocket::join_multicast(Ipv4Address group, Ipv4Address iface) {
    return set_membership(m_handle, m_uv_exec, group, iface, UV_JOIN_GROUP);
}

JoinHandle<void> UdpSocket::leave_multicast(Ipv4Address group, Ipv4Address iface) {
    return set_membership(m_handle, m_uv_exec, group, iface, UV_LEAVE_GROUP);
}
```

### `bind` and destructor

Same shape as `TcpListener::bind`/`~TcpListener`: `uv_udp_init` + `uv_udp_bind` on the uv
executor for `bind()`. `bind()` also caches `handle->raw_fd = uv_fileno(...)` at this
point — once, on the uv thread, right after the socket exists — so `recv_from_impl`'s
fast path never needs to call a libuv accessor off the uv thread later. Unlike an earlier
draft, `bind()` does **not** arm anything up front — nothing is armed until a
`recv_from()`/`recv()` call actually needs its slow path, and that arming happens inline
inside that call's own `with_context` hop (see [Receive path](#receive-path) above), not
as a separate step `bind()` triggers. The destructor closes `handle->handle`
asynchronously via `with_context(...).detach()`, same pattern as `TcpStream::~TcpStream()`.

```mermaid
sequenceDiagram
    participant C as coroutine (compute executor)
    participant W as with_context
    participant U as uv thread
    participant L as libuv

    Note over C,L: ... datagram arrives, nothing armed, kernel SO_RCVBUF holds it ...

    C->>C: co_await sock.recv_from(buf)
    C->>C: fast path: recvfrom() on raw_fd, directly on this thread
    C-->>C: data was already in the kernel buffer — resolves immediately, no suspension

    Note over C,L: ... later, kernel buffer is empty when recv_from() is called ...
    C->>C: fast path: recvfrom() returns EAGAIN
    C->>W: co_await with_context(uv_exec, ...) — hops and suspends until it resolves
    W->>U: arm: uv_udp_recv_start(alloc_cb, recv_cb), alloc_cb hands out buf directly

    Note over U,L: ... datagram arrives ...
    L->>U: alloc_cb: hands out caller's own buffer (no scratch, no copy)
    L->>U: recv_cb: uv_udp_recv_stop (single-shot); result.complete(nread, sender)
    U-->>C: with_context's JoinHandle resolves — coroutine resumes with the result
```

---

## Pico (lwIP) backend

lwIP's raw UDP API (`lwip/udp.h`) is markedly simpler than its TCP API: no connection
state machine, no send-buffer/window management, and — critically — `udp_sendto()` is
**synchronous**. It copies (or references, per `pbuf` type) the outgoing data and returns
immediately; there is no completion callback to await at all.

### `LwipUdpCtx` — internal shared state

Unlike the libuv backend, there's no raw-fd fast path here — lwIP has no socket fd and no
OS-level receive buffer beneath it, so every `recv_from()`/`recv()` call registers
`on_recv` fresh and suspends. Like the libuv `Handle`, there is exactly one in-flight
receive at a time, so these fields describe a single pending call, not a queue:

```cpp
// src/io/lwip/lwip_udp_ctx.h
namespace coro::detail {

struct LwipUdpCtx {
    udp_pcb*          pcb = nullptr;
    // Set by UdpSocket::connect() so send()/recv() can throw a clear error instead
    // of silently behaving like send_to()/recv_from() with no destination. Filtering
    // of non-peer datagrams is handled by lwIP itself once udp_connect() is called —
    // this flag exists only for that precondition check.
    bool connected = false;

    // Single in-flight receive — no queue. pending_buf is the caller's own buffer,
    // registered for the duration of one recv_from_impl() call; on_recv copies directly
    // into it and reports completion via result_ready.
    std::byte*    pending_buf  = nullptr;
    std::size_t   pending_len  = 0;
    bool          result_ready = false;
    std::size_t   result_len   = 0;
    SocketAddress result_sender;
    Rc<Waker>     rx_waker;

    static void on_recv(void* arg, udp_pcb* pcb, pbuf* p,
                         const ip_addr_t* addr, u16_t port);
};

} // namespace coro::detail
```

### Receive callback

```cpp
void LwipUdpCtx::on_recv(void* arg, udp_pcb* pcb, pbuf* p,
                          const ip_addr_t* addr, u16_t port) {
    auto* ctx = static_cast<LwipUdpCtx*>(arg);

    udp_recv(pcb, nullptr, nullptr);  // single-shot: deregister immediately

    std::size_t n = std::min(ctx->pending_len, static_cast<std::size_t>(p->tot_len));
    pbuf_copy_partial(p, ctx->pending_buf, n, 0);
    // p->tot_len > n means the datagram was truncated to fit the caller's buffer.
    pbuf_free(p);

    Ipv4Address v4;
    std::memcpy(v4.octets.data(), &addr->addr, 4);  // ip_addr_t is IPv4-only in this build (no LWIP_IPV6)

    ctx->result_len    = n;
    ctx->result_sender  = SocketAddress{v4, port};
    ctx->result_ready   = true;
    if (ctx->rx_waker) { auto w = std::move(ctx->rx_waker); w->wake(); }
}
```

`udp_recv()` registers this callback fresh at the start of every `recv_from_impl()` call
and it deregisters itself (`udp_recv(pcb, nullptr, nullptr)`) the instant a datagram
arrives — there is no "stays armed" mode. This means a datagram that arrives while no
`recv_from()`/`recv()` call is currently registered is simply dropped by lwIP: nothing
holds it, since lwIP (unlike a kernel socket) has no receive buffer of its own underneath
`on_recv`. See [Known limitations](#known-limitations--future-work).

### `recv_from_impl`

```cpp
template<ByteBuffer Buf>
Coro<std::tuple<std::size_t, Buf, SocketAddress>> UdpSocket::recv_from_impl(detail::Rc<detail::LwipUdpCtx> ctx, Buf buf) {
    struct DatagramReady {
        using OutputType = void;
        detail::Rc<detail::LwipUdpCtx> ctx;
        PollResult<void> poll(detail::Context& cx) {
            if (ctx->result_ready) return PollReady;
            // RACE CONDITION NOTE: safe — on_recv fires on the executor thread
            // (cyw43_arch_poll / sys_check_timeouts), never concurrently.
            ctx->rx_waker = cx.getWaker();
            return PollPending;
        }
    };

    ctx->pending_buf  = reinterpret_cast<std::byte*>(buf.data());
    ctx->pending_len  = buf.size();
    ctx->result_ready = false;
    udp_recv(ctx->pcb, &detail::LwipUdpCtx::on_recv, ctx.get());

    co_await DatagramReady{ctx};

    co_return {ctx->result_len, std::move(buf), ctx->result_sender};
}
```

### `send_to_impl`

No awaiting needed — `udp_sendto()` either copies the data into its own pbuf immediately
(with `PBUF_RAM`) or fails synchronously. `dest` must currently hold an `Ipv4Address` —
see [Known limitations](#known-limitations--future-work):

```cpp
Coro<void> UdpSocket::send_to_impl(const std::byte* buf, std::size_t size, SocketAddress dest) {
    if (!std::holds_alternative<Ipv4Address>(dest.address))
        throw std::runtime_error("UdpSocket::send_to: IPv6 destination not supported on the lwIP backend");
    const auto& v4 = std::get<Ipv4Address>(dest.address);

    pbuf* p = pbuf_alloc(PBUF_TRANSPORT, size, PBUF_RAM);
    if (!p) throw std::runtime_error("UdpSocket::send_to: pbuf_alloc failed (out of memory)");
    std::memcpy(p->payload, buf, size);

    ip_addr_t addr;
    IP4_ADDR(&addr, v4.octets[0], v4.octets[1], v4.octets[2], v4.octets[3]);

    err_t err = udp_sendto(m_impl->pcb, p, &addr, dest.port);
    pbuf_free(p);
    if (err != ERR_OK)
        throw std::runtime_error("UdpSocket::send_to: udp_sendto failed");
    co_return;
}
```

Because this never suspends, `send_to()` on the Pico backend completes synchronously in
practice — the `Coro<void>` return type is kept only for API symmetry with the libuv
backend and to leave room for a future flow-control mechanism (see below) without an
API break.

### `connect_impl`, `send_impl`, `recv_impl`

`udp_connect()` is also synchronous: it stores the peer's address/port on the `pcb` and
sets the `UDP_FLAGS_CONNECTED` flag, after which lwIP itself drops any datagram not from
that peer before `on_recv` ever fires — no filtering logic needed on the `coro` side.
`udp_send()` (vs. `udp_sendto()`) then reuses that stored peer:

```cpp
Coro<void> UdpSocket::connect_impl(SocketAddress peer) {
    if (!std::holds_alternative<Ipv4Address>(peer.address))
        throw std::runtime_error("UdpSocket::connect: IPv6 peer not supported on the lwIP backend");
    const auto& v4 = std::get<Ipv4Address>(peer.address);

    ip_addr_t addr;
    IP4_ADDR(&addr, v4.octets[0], v4.octets[1], v4.octets[2], v4.octets[3]);

    err_t err = udp_connect(m_impl->pcb, &addr, peer.port);
    if (err != ERR_OK)
        throw std::runtime_error("UdpSocket::connect: udp_connect failed");
    m_impl->connected = true;
    co_return;
}

Coro<Buf> UdpSocket::send_impl(Buf buf) {
    if (!m_impl->connected)
        throw std::runtime_error("UdpSocket::send: not connected — call connect() first");

    pbuf* p = pbuf_alloc(PBUF_TRANSPORT, buf.size(), PBUF_RAM);
    if (!p) throw std::runtime_error("UdpSocket::send: pbuf_alloc failed (out of memory)");
    std::memcpy(p->payload, buf.data(), buf.size());

    err_t err = udp_send(m_impl->pcb, p);  // no addr/port — uses the connected peer
    pbuf_free(p);
    if (err != ERR_OK)
        throw std::runtime_error("UdpSocket::send: udp_send failed");
    co_return std::move(buf);
}
```

`recv_impl` is `recv_from_impl` with the precondition check added and the `SocketAddress`
element of the tuple dropped — it is implemented in terms of `recv_from_impl` rather than
duplicated, same as the libuv backend's `recv()`.

### `set_broadcast_impl`, `join_multicast_impl`, `leave_multicast_impl`

None of these suspend, so — like `send_to_impl`/`connect_impl` — they're plain `Coro<void>`
kept synchronous in practice, with the `Coro` wrapper only for API symmetry with the
libuv backend:

```cpp
Coro<void> UdpSocket::set_broadcast_impl(bool enabled) {
    // No-op: IP_SOF_BROADCAST / IP_SOF_BROADCAST_RECV both default to 0 (lwIP's own
    // opt.h default, left unset in this project's lwipopts.h.in), so udp_sendto_if()
    // never checks an SOF_BROADCAST pcb flag in the first place — see
    // "Multicast and broadcast" above. Kept only for API symmetry with the libuv backend.
    (void)enabled;
    co_return;
}

Coro<void> UdpSocket::join_multicast_impl(Ipv4Address group, Ipv4Address iface) {
    (void)iface;  // Pico has exactly one network interface; always joins on netif_default
    ip4_addr_t addr;
    IP4_ADDR(&addr, group.octets[0], group.octets[1], group.octets[2], group.octets[3]);
    err_t err = igmp_joingroup_netif(netif_default, &addr);
    if (err != ERR_OK)
        throw std::runtime_error("UdpSocket::join_multicast: igmp_joingroup_netif failed");
    co_return;
}

Coro<void> UdpSocket::leave_multicast_impl(Ipv4Address group, Ipv4Address iface) {
    (void)iface;
    ip4_addr_t addr;
    IP4_ADDR(&addr, group.octets[0], group.octets[1], group.octets[2], group.octets[3]);
    err_t err = igmp_leavegroup_netif(netif_default, &addr);
    if (err != ERR_OK)
        throw std::runtime_error("UdpSocket::leave_multicast: igmp_leavegroup_netif failed");
    co_return;
}
```

Receiving multicast traffic needs no change to `on_recv`/`recv_from_impl` beyond the
IGMP join itself: `bind()` already binds the `pcb` to `IP_ADDR_ANY`, which (like the
libuv backend binding `0.0.0.0`) accepts a datagram addressed to any destination IP
matching the port — multicast included — once `igmp_joingroup_netif()` has told the
network layer to actually deliver that group's traffic up to IP.

### Destructor

```cpp
UdpSocket::~UdpSocket() {
    if (!m_impl || !m_impl->pcb) return;
    udp_recv(m_impl->pcb, nullptr, nullptr);  // detach callback before removal
    udp_remove(m_impl->pcb);
    m_impl->pcb = nullptr;
}
```

`udp_remove()` is synchronous and immediate — there's no FIN/graceful-close analogue for
UDP, so unlike `TcpStream`'s `tcp_close()`/`tcp_abort()` fallback, there's only one path.

```mermaid
sequenceDiagram
    participant C as coroutine
    participant L as lwIP
    participant E as CurrentThreadExecutor

    C->>L: udp_bind(pcb, addr, port)

    Note over C,L: ... nothing registered — a datagram arriving now is dropped ...

    C->>C: co_await recv_from(buf)
    C->>L: udp_recv(pcb, on_recv, ctx) — register for this call only
    C->>C: suspends, awaiting result_ready

    L->>L: on_recv: copy min(pending_len, p->tot_len) bytes into pending_buf
    L->>L: udp_recv(pcb, nullptr, nullptr) — deregister immediately (single-shot)
    L-->>C: result_ready = true; rx_waker->wake()
    E->>C: resumes, returns {result_len, buf, sender}

    C->>L: udp_sendto(pcb, pbuf, addr, port) — synchronous, no suspension
```

Once `connect()` has been called, the same diagram applies with `udp_connect(pcb, addr,
port)` run once up front and `udp_send(pcb, pbuf)` (no address) replacing `udp_sendto()` —
`on_recv` is unchanged; lwIP filters non-peer datagrams before it fires.

---

## Known limitations / Future work

- **Multicast/broadcast scope is deliberately narrow.** No IPv6 multicast (MLD) support —
  `join_multicast`/`leave_multicast` take `Ipv4Address`, not `SocketAddress`. No
  source-specific multicast (IGMPv3), no multicast TTL/loopback control, and no computed
  directed (subnet) broadcast addresses — callers must supply a literal broadcast address
  (e.g. `255.255.255.255`) themselves. On the Pico (lwIP) backend, `join_multicast`'s
  `iface` parameter is accepted but ignored — a Pico target has exactly one network
  interface, so `igmp_joingroup_netif()` always targets `netif_default`. See [Multicast
  and broadcast](#multicast-and-broadcast).
- **No userspace receive buffering on either backend — datagrams are dropped if nothing
  is awaiting `recv_from`/`recv` at the moment they arrive, on the Pico (lwIP) backend
  specifically.** On the desktop (libuv) backend this is a non-issue in practice: the
  kernel's own per-socket receive buffer (`SO_RCVBUF`) holds datagrams that arrive between
  calls, exactly as it would for any other UDP socket. lwIP has no equivalent — `on_recv`
  is only ever registered for the duration of one `recv_from_impl()` call, and a datagram
  arriving while nothing is registered is simply gone, with no buffer anywhere to catch
  it (see [Receive path](#receive-path) and the lwIP backend's `on_recv` above). This is a
  deliberate simplification (dropping order/backpressure semantics aren't a requirement
  right now) in exchange for a much smaller design, but is a genuine platform-specific
  behavior difference callers relying on this backend need to be aware of — bursty senders
  faster than the receiver's polling cadence will lose datagrams on Pico that an equivalent
  desktop program would not.
- **`send_to()` throws unconditionally after `connect()` on the libuv/Linux backend
  (even to the connected peer's own address), but succeeds on lwIP.** See the warning
  in [Mixing `_to`/`_from` calls with `connect()`](#connect-send-recv-fixed-peer-mode) —
  this is a genuine kernel (`EISCONN`) vs. lwIP behavior asymmetry, not a bug to be
  fixed; callers on the libuv backend must use `send()` once connected, and callers who
  need portable arbitrary-peer addressing after `connect()` should use a second,
  unconnected socket instead.
- **`SocketAddress` supports IPv6 (with `scope_id`), but the lwIP/Pico backend does
  not.** `send_to`/`recv_from`/`connect` throw at runtime if given an `Ipv6Address` on
  that backend, consistent with the existing IPv4-only `TcpStream`/`TcpListener`
  limitation noted in `pico_port.md`. The libuv (desktop) backend handles both families
  via `sockaddr_storage`, but `UdpSocket::bind()` accepting an IPv6 host and constructing
  an `AF_INET6` socket is not yet wired up — worth a follow-up once there's a concrete
  IPv6 caller.

!!! tip "TODO: apply the same raw-fd fast path to `TcpStream`"
    `TcpStream::read()` today always hops to the uv thread via `with_context`, even when
    data is already sitting in the kernel's receive buffer and could be read immediately
    on the calling thread via `uv_fileno()` — the same opportunity `UdpSocket::recv_from()`
    now takes advantage of. Encouragingly, `UdpSocket`'s own safety argument (see
    [Fast path safety](#fast-path-safety-mutually-exclusive-by-construction) above) no
    longer relies on anything UDP-specific — it falls out of per-call arm/disarm plus the
    existing one-read-at-a-time restriction, both of which `TcpStream` already has or could
    adopt. So the argument likely *does* transfer to `TcpStream` fairly directly once
    attempted, which is a stronger starting point than assumed in an earlier draft of this
    note — but it still needs its own pass to confirm (e.g. checking level-triggered
    readiness re-checking behaves correctly across the two paths), not simply asserted by
    analogy. Deliberately out of scope for this design: no changes to `TcpStream` or any
    other existing code until the `UdpSocket` design above is implemented and tested.

---

## File structure

```
include/coro/io/
  socket_address.h         SocketAddress/Ipv4Address/Ipv6Address — shared address type (both backends)
  socket_address_uv.h      to_sockaddr()/from_sockaddr() conversion helpers (libuv only) —
                           lives here, not src/io/, since udp_socket.hpp (public, template
                           method bodies) needs it
  udp_socket.h             UdpSocket — dispatches to lwIP or libuv backend
  udp_socket.hpp           send_to<Buf>()/recv_from<Buf>()/send<Buf>()/recv<Buf>() template
                           impls (libuv); included from the bottom of udp_socket.h

src/io/
  socket_address.cpp       SocketAddress::parse()/to_string() (both backends)
  udp_socket.cpp           bind() / connect() / recv_from_impl() / destructor /
                           set_broadcast() / join_multicast() / leave_multicast() (libuv)
  lwip/
    lwip_udp_ctx.h          LwipUdpCtx internal struct
    udp_socket_lwip.cpp     UdpSocket implementation (lwIP)
```

---

## Status

Design complete, including `SocketAddress`, fixed-peer `connect()`/`send()`/`recv()`, and
a no-userspace-buffering receive path: no internal queue on either backend, relying
instead on the kernel's own per-socket receive buffer on the desktop (libuv) backend, and
on per-call `on_recv` registration (dropping datagrams that arrive while idle) on the Pico
(lwIP) backend. This replaces two earlier, successively-abandoned designs — a
`BufferPool`/`PooledBuffer` draft, then a `PollStream`-style `RingBuffer<DatagramEntry>` +
`BackpressureMode` draft — in favor of this simpler no-buffering approach. The libuv
backend additionally gets a raw-fd fast path that lets `recv_from()`/`recv()` resolve
immediately, with no uv-thread hop and no extra copy, whenever a datagram is already
sitting in the kernel's receive buffer; only the (now single-shot, per-call) slow path
touches the uv thread. Multicast (`join_multicast`/`leave_multicast`, via
`uv_udp_set_membership()`/`igmp_joingroup_netif()`) and broadcast (`set_broadcast()`, via
`uv_udp_set_broadcast()` on libuv, a no-op on lwIP) are included in this first iteration —
see [Multicast and broadcast](#multicast-and-broadcast) — rather than deferred.

**Implemented and tested.** Both backends are in place per the design above:
`include/coro/io/socket_address.h` + `src/io/socket_address.cpp` (shared), and the
libuv/lwIP `UdpSocket` split described in [File structure](#file-structure). Real gtest
coverage exists for both: `test/io/test_udp_socket.cpp` runs against the desktop libuv
backend (`SocketAddress` parse/format round trips; `send_to`/`recv_from`; truncation of
oversized datagrams; `connect`/`send`/`recv`; mixing `send_to`/`recv_from` with a
connected socket; `set_broadcast`; `join_multicast`/`leave_multicast`, including an actual
multicast loopback delivery test), and `test/pico/test_udp_socket_real.cpp` runs the
same core scenarios against real lwIP in NO_SYS mode over the host loopback netif
(multicast excluded there — lwIP's default loopback netif doesn't set `NETIF_FLAG_IGMP`,
so `igmp_joingroup_netif()` isn't exercisable against it regardless of `UdpSocket`'s own
correctness). CMake wiring: `src/io/socket_address.cpp` and `src/io/udp_socket.cpp` are
part of the desktop `coro` target; `cmake/platforms/pico.cmake`'s `coro_pico` target
additionally defines `CORO_UDP_BACKEND_LWIP` and compiles `socket_address.cpp` +
`udp_socket_lwip.cpp`; `test/CMakeLists.txt` adds `test_udp_socket` (desktop) and a new
`coro_lwip_udp` library + `test_udp_socket_real` executable (real lwIP), plus
`igmp.c` in the `lwip_host` source list and `LWIP_IGMP 1` in the host test
`lwipopts.h` (needed for the library to link `join_multicast`/`leave_multicast`'s
`igmp_joingroup_netif`/`igmp_leavegroup_netif` calls, even though that path isn't
exercised by the real-lwIP test itself).
