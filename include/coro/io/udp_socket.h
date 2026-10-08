#pragma once

#ifdef CORO_UDP_BACKEND_LWIP

// ---------------------------------------------------------------------------
// lwIP-backed UdpSocket (CORO_UDP_BACKEND_LWIP)
//
// Backed by the lwIP raw UDP API in NO_SYS mode. All callbacks fire
// synchronously on the executor thread during cyw43_arch_poll() /
// sys_check_timeouts(). No lwIP headers appear here — the implementation is
// compiled separately via src/io/lwip/udp_socket_lwip.cpp.
// ---------------------------------------------------------------------------

#include <coro/coro.h>
#include <coro/io/byte_buffer.h>
#include <coro/io/socket_address.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/rc.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace coro {

namespace detail {

struct LwipUdpCtx;

// The non-template halves of the send and receive futures below. Defined in
// udp_socket_lwip.cpp, which keeps the lwIP headers out of this file.

/// Sends one datagram to `dest`, or to the connected peer if `dest` is null. lwIP
/// copies it into a pbuf and hands it to the interface before returning, so the
/// result is ready or an error, never pending.
PollResult<void> lwip_udp_send(LwipUdpCtx& udp, const std::byte* buf, std::size_t size,
                               const SocketAddress* dest);

/// One poll of a receive. The first call (`armed` false) registers the receive
/// callback to copy the next datagram into `buf`, and sets `armed`. Ready, with `n`
/// and `sender` filled in and `armed` cleared, once that datagram has arrived.
/// `connected_only` makes the first call fail unless connect() has been called.
PollResult<void> lwip_udp_poll_recv(LwipUdpCtx& udp, std::byte* buf, std::size_t size,
                                    bool connected_only, bool& armed, std::size_t& n,
                                    SocketAddress& sender, Context& ctx);

/// Deregisters the receive callback and drops the receive waker, for an armed
/// receive that is destroyed before it completes.
void lwip_udp_cancel_recv(LwipUdpCtx& udp) noexcept;

} // namespace detail

/**
 * @brief Future returned by `UdpSocket::send()` / `send_to()`. Yields `buf`.
 *
 * Hand-written rather than a `Coro<Buf>`, so a send has no coroutine frame and
 * allocates nothing of its own. An lwIP send never waits: the first `poll()` hands
 * the datagram to lwIP and returns ready, or an error.
 */
template<ByteBuffer Buf>
class UdpSendFuture {
public:
    using OutputType = Buf;

    UdpSendFuture(detail::Rc<detail::LwipUdpCtx> udp, Buf buf,
                  std::optional<SocketAddress> dest)
        : m_udp(std::move(udp)), m_buf(std::move(buf)), m_dest(dest) {}

    PollResult<Buf> poll(detail::Context&) {
        auto result = detail::lwip_udp_send(*m_udp,
            reinterpret_cast<const std::byte*>(std::ranges::data(m_buf)),
            std::ranges::size(m_buf), m_dest ? &*m_dest : nullptr);
        if (result.isError()) return PollError(result.error());
        return std::move(m_buf);
    }

private:
    detail::Rc<detail::LwipUdpCtx> m_udp;
    Buf                            m_buf;
    std::optional<SocketAddress>   m_dest;   // nullopt => send() on a connected socket
};

/**
 * @brief Future returned by `UdpSocket::recv_from()` (`WithSender = true`, yields
 * `{n, buf, sender}`) and `UdpSocket::recv()` (`WithSender = false`, yields `{n, buf}`).
 *
 * Hand-written rather than a `Coro`, so a receive has no coroutine frame and
 * allocates nothing. The first `poll()` registers lwIP's receive callback, which
 * copies the next datagram straight into this future's buffer. Nothing is registered
 * before that poll, and a datagram that arrives while no receive is registered is
 * dropped.
 *
 * Dropping it while pending is safe: the destructor deregisters the callback, so
 * lwIP never writes into a buffer that has gone. A datagram that had already been
 * copied into the dropped future is lost.
 *
 * @warning Must not be moved once polled. The callback holds a pointer into this
 * future's buffer, which for an inline buffer (`std::array`, a short `std::string`)
 * moves with the future. Every future is pinned after its first poll; here a
 * violation would be a wild write.
 *
 * Race note: the callback runs on the executor thread, never concurrently with
 * `poll()`. Overlapping receives on one socket aren't supported: the second would
 * take over the callback and the first would never complete.
 */
template<ByteBuffer Buf, bool WithSender>
class UdpRecvFuture {
public:
    using OutputType = std::conditional_t<WithSender,
        std::tuple<std::size_t, Buf, SocketAddress>, std::pair<std::size_t, Buf>>;

    UdpRecvFuture(detail::Rc<detail::LwipUdpCtx> udp, Buf buf)
        : m_udp(std::move(udp)), m_buf(std::move(buf)) {}

    // For the move before the first poll. A moved-from future has no socket, so its
    // destructor does nothing.
    UdpRecvFuture(UdpRecvFuture&&) noexcept            = default;
    UdpRecvFuture& operator=(UdpRecvFuture&&) noexcept = delete;

    ~UdpRecvFuture() {
        if (m_udp && m_armed) detail::lwip_udp_cancel_recv(*m_udp);
    }

    PollResult<OutputType> poll(detail::Context& ctx) {
        std::size_t   n = 0;
        SocketAddress sender;
        auto result = detail::lwip_udp_poll_recv(*m_udp,
            reinterpret_cast<std::byte*>(std::ranges::data(m_buf)), std::ranges::size(m_buf),
            /*connected_only=*/!WithSender, m_armed, n, sender, ctx);
        if (result.isPending()) return PollPending;
        if (result.isError())   return PollError(result.error());
        if constexpr (WithSender) return OutputType{n, std::move(m_buf), sender};
        else                      return OutputType{n, std::move(m_buf)};
    }

private:
    detail::Rc<detail::LwipUdpCtx> m_udp;
    Buf                            m_buf;
    // True while lwIP's receive callback points at m_buf.
    bool                           m_armed = false;
};

/**
 * @brief Async, connectionless UDP socket. Obtain via `co_await UdpSocket::bind()`.
 *
 * See doc/design/udp_socket.md. There is no internal receive queue: a datagram
 * that arrives while nothing is awaiting `recv_from()`/`recv()` is dropped by
 * lwIP, since (unlike the desktop backend) there is no OS-level receive buffer
 * underneath it.
 *
 * **Concurrency:** only one receive (`recv_from()`/`recv()`) and only one send
 * (`send_to()`/`send()`) may be in flight at a time; `connect()` must not run
 * concurrently with either.
 *
 * **Cancellation:** dropping a pending send or receive is always safe.
 */
class UdpSocket {
public:
    UdpSocket(UdpSocket&&) noexcept;
    UdpSocket& operator=(UdpSocket&&) noexcept;
    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    ~UdpSocket();

    /// Binds a UDP socket to host:port. IPv4 only on this backend — host must be
    /// dotted-decimal or "0.0.0.0".
    [[nodiscard]] static Coro<UdpSocket> bind(std::string host, uint16_t port);

    /// Sends buf as a single datagram to dest. Returns buf once the send completes.
    /// @throws std::runtime_error (at co_await) if dest is IPv6 or lwIP rejects it.
    template<ByteBuffer Buf>
    [[nodiscard]] UdpSendFuture<Buf> send_to(Buf buf, SocketAddress dest) {
        return UdpSendFuture<Buf>(m_impl, std::move(buf), dest);
    }

    /// Waits for the next datagram, copying it into buf. Returns {n, buf, sender}.
    /// A datagram longer than buf is truncated to fit.
    template<ByteBuffer Buf>
    [[nodiscard]] UdpRecvFuture<Buf, true> recv_from(Buf buf) {
        return UdpRecvFuture<Buf, true>(m_impl, std::move(buf));
    }

    /// Fixes peer as this socket's only correspondent.
    [[nodiscard]] Coro<void> connect(SocketAddress peer);

    /// Sends buf to the peer fixed by connect().
    /// @throws std::runtime_error (at co_await) if not connected.
    template<ByteBuffer Buf>
    [[nodiscard]] UdpSendFuture<Buf> send(Buf buf) {
        return UdpSendFuture<Buf>(m_impl, std::move(buf), std::nullopt);
    }

    /// Waits for the next datagram from the peer fixed by connect().
    /// @throws std::runtime_error (at co_await) if not connected.
    template<ByteBuffer Buf>
    [[nodiscard]] UdpRecvFuture<Buf, false> recv(Buf buf) {
        return UdpRecvFuture<Buf, false>(m_impl, std::move(buf));
    }

    /// No-op on this backend — see doc/design/udp_socket.md's "Multicast and
    /// broadcast" section. Kept for API symmetry with the desktop backend.
    [[nodiscard]] Coro<void> set_broadcast(bool enabled);

    /// Joins a multicast group via igmp_joingroup_netif(). iface is accepted for
    /// API symmetry with the desktop backend but ignored — a Pico target has
    /// exactly one network interface (netif_default).
    [[nodiscard]] Coro<void> join_multicast(Ipv4Address group, Ipv4Address iface = {});

    /// Leaves a multicast group previously joined with join_multicast().
    [[nodiscard]] Coro<void> leave_multicast(Ipv4Address group, Ipv4Address iface = {});

private:
    explicit UdpSocket(detail::Rc<detail::LwipUdpCtx> impl);

    detail::Rc<detail::LwipUdpCtx> m_impl;
};

} // namespace coro

#else // !CORO_UDP_BACKEND_LWIP — desktop implementation on the IoDriver

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/socket_state.h>
#include <coro/io/byte_buffer.h>
#include <coro/io/socket_address.h>
#include <coro/runtime/io_driver.h>
#include <coro/coro.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace coro {

template<ByteBuffer Buf>
class UdpSendFuture;

template<ByteBuffer Buf, bool WithSender>
class UdpRecvFuture;

template<ByteBuffer Buf>
class UdpRecvSegmentsFuture;

/// Result of UdpSocket::recv_segments_from(): buf[0, size) holds one or more
/// datagrams from sender, back to back, each segment_size bytes except that the
/// last may be shorter. segment_size == size when the kernel did not coalesce.
template<ByteBuffer Buf>
struct UdpSegments {
    std::size_t   size;
    Buf           buf;
    SocketAddress sender;
    std::size_t   segment_size;
};


/**
 * @brief Async, connectionless UDP socket. Obtain via `co_await UdpSocket::bind()`.
 *
 * See doc/design/udp_socket.md, "Desktop (IoDriver) backend". Every
 * operation is a non-blocking syscall on the calling thread; a send or receive that
 * would block waits for readiness from the Runtime's IoDriver. There is no internal
 * receive queue: datagrams that arrive between calls wait in the kernel's own
 * per-socket receive buffer.
 *
 * Requires an executor that turns the IoDriver (`Runtime(n)` for any n);
 * `bind()` throws `std::logic_error` otherwise.
 *
 * **Concurrency:** only one receive (`recv_from()`/`recv()`/`recv_segments_from()`)
 * and only one send (`send_to()`/`send()`) may be in flight at a time; a receive and
 * a send may run concurrently, on any threads. `connect()` must not run
 * concurrently with either.
 *
 * **Cancellation:** dropping a pending send or receive is always safe; no datagram
 * is consumed by a dropped receive.
 */
class UdpSocket {
public:
    UdpSocket(UdpSocket&&) noexcept;
    UdpSocket& operator=(UdpSocket&&) noexcept;
    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// Releases this handle. The socket is deregistered and closed synchronously once
    /// no in-flight future still uses it.
    ~UdpSocket();

    /// Binds a UDP socket to host:port. host is an IPv4 or IPv6 literal ("0.0.0.0",
    /// "127.0.0.1", "::", "::1") or a name resolved with lookup_host(); the first
    /// resolved address that binds is used.
    /// @throws std::system_error (at co_await) if host doesn't resolve, or with the last
    ///         address's bind() error.
    /// @throws std::logic_error if the current Runtime's executor doesn't turn the
    ///         IoDriver (a CurrentThreadExecutor given its own Parker).
    [[nodiscard]] static Coro<UdpSocket> bind(std::string host, uint16_t port);

    /// Sends buf as a single datagram to dest. Returns buf once the send completes.
    ///
    /// Returns a hand-written future (not a Coro): when the kernel accepts the
    /// datagram at once, the first poll() completes it with no allocation.
    template<ByteBuffer Buf>
    [[nodiscard]] UdpSendFuture<Buf> send_to(Buf buf, SocketAddress dest);

    /// Waits for the next datagram, copying it into buf. Returns {n, buf, sender}.
    /// Oversized datagrams are truncated to fit buf, same as POSIX recvfrom().
    template<ByteBuffer Buf>
    [[nodiscard]] UdpRecvFuture<Buf, true> recv_from(Buf buf);

    /// Fixes peer as this socket's only correspondent. Once connected, plain
    /// send()/recv() may be used instead of send_to()/recv_from(); datagrams from
    /// any other address are dropped by the OS before they ever reach recv().
    /// Note: on Linux, send_to() with an explicit destination remains usable
    /// even after connect() -- the kernel does not reject it (see
    /// doc/design/udp_socket.md's "Known limitations" section).
    [[nodiscard]] Coro<void> connect(SocketAddress peer);

    /// Sends buf to the peer fixed by connect(). Throws (EDESTADDRREQ) if not connected.
    template<ByteBuffer Buf>
    [[nodiscard]] UdpSendFuture<Buf> send(Buf buf);

    /// Waits for the next datagram from the peer fixed by connect(), copying it into buf.
    template<ByteBuffer Buf>
    [[nodiscard]] UdpRecvFuture<Buf, false> recv(Buf buf);

    /// Enables (or disables) sending to broadcast addresses via send_to()/send().
    /// Required before a sendto() to a broadcast address is permitted by the OS
    /// (SO_BROADCAST) — otherwise it fails with EACCES.
    [[nodiscard]] Coro<void> set_broadcast(bool enabled);

    /// Enables UDP generic segmentation offload (GSO, Linux UDP_SEGMENT) for
    /// every later send()/send_to(): a buffer longer than bytes goes out as
    /// consecutive datagrams of bytes each (the last may be shorter), for one
    /// syscall and one pass through the stack. A buffer no longer than bytes is
    /// sent as a single datagram as usual. 0 disables. One buffer may carry at
    /// most 65507 bytes and 64 segments (128 on newer kernels); larger sends fail
    /// with EMSGSIZE. Sends through an interface without checksum offload fail
    /// with EIO.
    /// Synchronous: a plain setsockopt() on the socket.
    /// Throws std::system_error (ENOTSUP) on platforms other than Linux.
    void set_segment_size(std::size_t bytes);

    /// Enables UDP generic receive offload (GRO, Linux UDP_GRO): the kernel may
    /// then coalesce consecutive same-sized datagrams from one sender into a
    /// single queued buffer, which uses less of the socket receive buffer and
    /// is read with one syscall. Read such a socket with recv_segments_from()
    /// -- plain recv_from()/recv() would return the coalesced datagrams as one
    /// with no way to split them. Datagrams sent with GSO (set_segment_size())
    /// stay coalesced end to end on loopback; from a NIC, the driver's GRO
    /// coalesces them.
    /// Synchronous: a plain setsockopt() on the socket.
    /// Throws std::system_error (ENOTSUP) on platforms other than Linux.
    void set_gro(bool enabled);

    /// Like recv_from(), but for a socket with set_gro(true): returns one or
    /// more datagrams from one sender and the size to split them at (see
    /// UdpSegments). buf should hold 65535 bytes: a coalesced read that doesn't
    /// fit is truncated, losing whole datagrams. On a socket without GRO (or
    /// off Linux) every read is one datagram with segment_size == size.
    template<ByteBuffer Buf>
    [[nodiscard]] UdpRecvSegmentsFuture<Buf> recv_segments_from(Buf buf);

    /// Joins multicast group so recv_from()/recv() start receiving datagrams sent
    /// to it. iface selects which local interface to join on; the default
    /// (Ipv4Address{}) lets the OS choose. IPv4 only.
    [[nodiscard]] Coro<void> join_multicast(Ipv4Address group, Ipv4Address iface = {});

    /// Leaves a multicast group previously joined with join_multicast().
    [[nodiscard]] Coro<void> leave_multicast(Ipv4Address group, Ipv4Address iface = {});

private:
    using State = detail::SocketState;

    explicit UdpSocket(std::shared_ptr<State> state);

    // The coroutine bodies of the setup calls. Static, taking the state by value, so
    // the coroutine frame never holds `this` (the UdpSocket may move before it runs).
    static Coro<void> connect_impl(std::shared_ptr<State> state, SocketAddress peer);
    static Coro<void> set_broadcast_impl(std::shared_ptr<State> state, bool enabled);
    static Coro<void> set_membership_impl(std::shared_ptr<State> state, Ipv4Address group,
                                          Ipv4Address iface, bool join);

    std::shared_ptr<State> m_state;
};

} // namespace coro

#include <coro/io/udp_socket.hpp>

#endif // CORO_UDP_BACKEND_LWIP
