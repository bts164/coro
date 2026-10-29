#pragma once

// The lwIP sys backend's objects: what a POSIX kernel would hold for each open file.
// Private to src/detail/sys/lwip/. See doc/design/pico_port.md, "The lwIP sys backend".
//
// Threading: every IoObject is touched only on the executor's thread (lwIP runs
// there, inside Poller::poll()), except LwipNotify::context and its worker, which
// notify_signal() reads from interrupt handlers.

#include <coro/detail/sys/poller.h>
#include <coro/io/circular_byte_buffer.h>
#include <coro/io/socket_address.h>

#include <lwip/err.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <pico/async_context.h>

// lwIP defines tcp_listen(pcb) as a macro for tcp_listen_with_backlog(), which would
// break the declaration of the seam's own sys::tcp_listen(local, backlog). The
// backend calls tcp_listen_with_backlog() directly.
#ifdef tcp_listen
#undef tcp_listen
#endif

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace coro::detail::sys {

/// The base of every lwIP backend object; `RawFd` points to one. Non-polymorphic:
/// close_object() switches on `kind`.
struct IoObject {
    enum class Kind : uint8_t { Tcp, TcpListener, Udp, DnsQuery, Notify };

    // Bits of `flags`. The first four are readiness seen since the object was last
    // reported, set by lwIP callbacks and workers; kQueued means it is on the
    // Poller's ready list. One byte, so one memory location: only the executor's
    // thread may write it (see doc/design/pico_port.md, "IoObject and RawFd").
    static constexpr uint8_t kReadable  = 1u << 0;
    static constexpr uint8_t kWritable  = 1u << 1;
    static constexpr uint8_t kError     = 1u << 2;
    static constexpr uint8_t kHup       = 1u << 3;
    static constexpr uint8_t kQueued    = 1u << 4;
    static constexpr uint8_t kReadiness = kReadable | kWritable | kError | kHup;

    explicit IoObject(Kind k) : kind(k) {}
    IoObject(const IoObject&)            = delete;
    IoObject& operator=(const IoObject&) = delete;

    // Set by Poller::register_fd(): the ScheduledIo* reported in this object's
    // Events. Null while unregistered.
    void*     key        = nullptr;
    // The Poller's ready list.
    IoObject* next_ready = nullptr;
    // The Poller this object is registered with; null while unregistered.
    Poller*   poller     = nullptr;
    Kind      kind;
    uint8_t   flags      = 0;
};

/// Records readiness on `obj` from an lwIP callback or a worker. Reports nothing
/// while the object is unregistered: a new registration starts ready anyway.
inline void mark_ready(IoObject* obj, uint8_t bits) noexcept {
    if (obj->poller != nullptr)
        obj->poller->mark_ready(obj, bits);
    else
        obj->flags = static_cast<uint8_t>(obj->flags | (bits & IoObject::kReadiness));
}

// ---------------------------------------------------------------------------
// Socket objects
// ---------------------------------------------------------------------------

/// A TCP connection (connecting, connected, or failed).
struct LwipTcp : IoObject {
    LwipTcp() : IoObject(Kind::Tcp) {}

    tcp_pcb* pcb = nullptr;      // null once lwIP has freed it (tcp_err) or after close
    // Received bytes not yet read. Allocated by the first data, TCP_WND bytes:
    // lwIP's receive window, reopened only as reads take bytes (tcp_recved()),
    // stops the peer before it can overflow.
    std::optional<CircularByteBuffer> rx;
    bool connected = false;      // the handshake completed
    bool connecting = false;     // tcp_connect() issued, no answer yet
    bool eof       = false;      // the peer closed its side
    bool write_waiting = false;  // a write saw EAGAIN; tcp_poll re-checks
    int  error     = 0;          // pending errno, reported once the buffer is empty
};

/// A listening TCP socket and its accepted, not yet accept()ed connections.
struct LwipListener : IoObject {
    LwipListener() : IoObject(Kind::TcpListener) {}

    tcp_pcb*            pcb = nullptr;
    std::deque<LwipTcp*> queue;   // owned until tcp_try_accept() hands one out
};

/// A UDP socket. Received datagrams wait in `rx` as records: a RecordHeader, then
/// the payload.
struct LwipUdp : IoObject {
    LwipUdp() : IoObject(Kind::Udp) {}

    struct RecordHeader {
        uint16_t length;
        uint16_t port;
        uint8_t  addr[4];
    };

    udp_pcb* pcb = nullptr;
    std::optional<CircularByteBuffer> rx;
    bool connected = false;
};

/// A DNS lookup started by dns_start(). lwIP's DNS has no cancel, so a query closed
/// while pending is only marked orphaned, and its found callback deletes it.
struct LwipDnsQuery : IoObject {
    LwipDnsQuery() : IoObject(Kind::DnsQuery) {}

    bool      done     = false;
    bool      found    = false;
    bool      orphaned = false;
    ip_addr_t addr{};
};

/// A notify object (sys/notify.h): readiness from notify_signal(), through its own
/// async_context worker. `context` is set while registered.
struct LwipNotify : IoObject {
    LwipNotify();

    async_when_pending_worker_t worker{};
    // Read by notify_signal() from interrupt handlers: a plain pointer, written only
    // on the executor's thread while no signal can be relying on it. RACE: an ISR on
    // the other core reading it as it is cleared; see doc/design/isr_safety.md,
    // "The wait".
    async_context* volatile context = nullptr;
};

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

/// True while a Runtime with lwIP running exists (PicoNetwork::Cyw43 or Lwip). Set
/// by the Poller. Sockets and DNS queries refuse with ENETDOWN otherwise.
bool lwip_running() noexcept;
void set_lwip_running(bool running) noexcept;

/// Throws std::system_error with ENETDOWN unless lwip_running().
void require_lwip(const char* what);

/// lwIP's err_t as an errno, following lwIP's own err_to_errno() table.
int lwip_errno(err_t err) noexcept;

/// Throws std::system_error for errno `err`.
[[noreturn]] void throw_errno(int err, const char* what);

/// SocketAddress <-> lwIP address. to_ip_addr() fails for IPv6 (no LWIP_IPV6).
bool to_ip_addr(const SocketAddress& addr, ip_addr_t& out) noexcept;
SocketAddress from_ip_addr(const ip_addr_t& addr, uint16_t port) noexcept;

/// Writes all of `data` into `ring`; the caller has checked it fits.
inline void ring_write(CircularByteBuffer& ring, const void* data, std::size_t size) {
    const auto* src = static_cast<const std::byte*>(data);
    while (size > 0) {
        auto span = ring.writable_span();
        const std::size_t n = std::min(size, span.size());
        std::memcpy(span.data(), src, n);
        ring.commit_write(n);
        src += n;
        size -= n;
    }
}

/// Copies a whole pbuf chain into `ring`; the caller has checked it fits.
inline void ring_write(CircularByteBuffer& ring, const pbuf* p) {
    for (const pbuf* q = p; q != nullptr; q = q->next)
        ring_write(ring, q->payload, q->len);
}

/// Moves `size` bytes out of `ring` into `data` (null: discards them).
inline void ring_read(CircularByteBuffer& ring, void* data, std::size_t size) {
    auto* dst = static_cast<std::byte*>(data);
    while (size > 0) {
        auto span = ring.readable_span();
        const std::size_t n = std::min(size, span.size());
        if (dst != nullptr) {
            std::memcpy(dst, span.data(), n);
            dst += n;
        }
        ring.consume(n);
        size -= n;
    }
}

// ---------------------------------------------------------------------------
// Teardown, deferred out of the Poller's service pass
// ---------------------------------------------------------------------------
//
// Code that runs inside a service pass (async_context_poll() and the loopback drain)
// can drop the last reference to an object: a wake there can drop the last reference
// to a task, whose futures close their sockets. Removing a worker from the SDK's
// list while async_context_poll() walks it, or closing a pcb inside lwIP's own
// callback, is not safe, so both wait until the pass ends. See
// doc/design/pico_port.md, "Poller". Module-level state: a Pico has one thread, so
// one pass at a time.

/// True while a Poller's service pass runs.
bool in_service() noexcept;

/// Removes a deregistered notify object's worker from `context`: now, or when the
/// current service pass ends.
void detach_notify_deferred(LwipNotify* notify, async_context* context) noexcept;

/// Releases an object's lwIP resources and deletes it: now, or when the current
/// service pass ends. close_socket() calls it.
void close_object_deferred(IoObject* obj) noexcept;

// The immediate forms, defined with each kind's other functions.
void close_tcp(LwipTcp* tcp) noexcept;
void close_listener(LwipListener* listener) noexcept;
void close_udp(LwipUdp* udp) noexcept;
void close_dns_query(LwipDnsQuery* query) noexcept;
void close_notify(LwipNotify* notify) noexcept;

// A notify object's worker joins / leaves a context. Executor thread only, and
// detach never inside a service pass (detach_notify_deferred() handles that).
void attach_notify(LwipNotify* notify, async_context* context) noexcept;
void detach_notify(LwipNotify* notify, async_context* context) noexcept;

} // namespace coro::detail::sys
