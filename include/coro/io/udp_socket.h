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
#include <coro/detail/rc.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>

namespace coro {

namespace detail { struct LwipUdpCtx; }

/**
 * @brief Async, connectionless UDP socket. Obtain via `co_await UdpSocket::bind()`.
 *
 * See doc/design/udp_socket.md. There is no internal receive queue: a datagram
 * that arrives while nothing is awaiting `recv_from()`/`recv()` is dropped by
 * lwIP, since (unlike the libuv backend) there is no OS-level receive buffer
 * underneath it.
 *
 * **Concurrency:** only one receive (`recv_from()`/`recv()`) and only one send
 * (`send_to()`/`send()`) may be in flight at a time; `connect()` must not run
 * concurrently with either.
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
    template<ByteBuffer Buf>
    [[nodiscard]] Coro<Buf> send_to(Buf buf, SocketAddress dest) {
        co_await send_to_impl(reinterpret_cast<const std::byte*>(std::ranges::data(buf)),
                               std::ranges::size(buf), dest);
        co_return std::move(buf);
    }

    /// Waits for the next datagram, copying it into buf. Returns {n, buf, sender}.
    template<ByteBuffer Buf>
    [[nodiscard]] Coro<std::tuple<std::size_t, Buf, SocketAddress>> recv_from(Buf buf) {
        auto [n, sender] = co_await recv_from_impl(
            reinterpret_cast<std::byte*>(std::ranges::data(buf)), std::ranges::size(buf));
        co_return std::tuple<std::size_t, Buf, SocketAddress>{n, std::move(buf), sender};
    }

    /// Fixes peer as this socket's only correspondent.
    [[nodiscard]] Coro<void> connect(SocketAddress peer);

    /// Sends buf to the peer fixed by connect(). Throws if not connected.
    template<ByteBuffer Buf>
    [[nodiscard]] Coro<Buf> send(Buf buf) {
        co_await send_impl(reinterpret_cast<const std::byte*>(std::ranges::data(buf)),
                            std::ranges::size(buf));
        co_return std::move(buf);
    }

    /// Waits for the next datagram from the peer fixed by connect(). Throws if
    /// not connected.
    template<ByteBuffer Buf>
    [[nodiscard]] Coro<std::pair<std::size_t, Buf>> recv(Buf buf) {
        std::size_t n = co_await recv_impl(
            reinterpret_cast<std::byte*>(std::ranges::data(buf)), std::ranges::size(buf));
        co_return std::pair<std::size_t, Buf>{n, std::move(buf)};
    }

    /// No-op on this backend — see doc/design/udp_socket.md's "Multicast and
    /// broadcast" section. Kept for API symmetry with the libuv backend.
    [[nodiscard]] Coro<void> set_broadcast(bool enabled);

    /// Joins a multicast group via igmp_joingroup_netif(). iface is accepted for
    /// API symmetry with the libuv backend but ignored — a Pico target has
    /// exactly one network interface (netif_default).
    [[nodiscard]] Coro<void> join_multicast(Ipv4Address group, Ipv4Address iface = {});

    /// Leaves a multicast group previously joined with join_multicast().
    [[nodiscard]] Coro<void> leave_multicast(Ipv4Address group, Ipv4Address iface = {});

private:
    explicit UdpSocket(detail::Rc<detail::LwipUdpCtx> impl);

    // Defined in udp_socket_lwip.cpp. Never inline — keeps lwIP headers out of
    // this file.
    [[nodiscard]] Coro<void> send_to_impl(const std::byte* buf, std::size_t size, SocketAddress dest);
    [[nodiscard]] Coro<std::tuple<std::size_t, SocketAddress>> recv_from_impl(std::byte* buf, std::size_t size);
    [[nodiscard]] Coro<void> send_impl(const std::byte* buf, std::size_t size);
    [[nodiscard]] Coro<std::size_t> recv_impl(std::byte* buf, std::size_t size);

    detail::Rc<detail::LwipUdpCtx> m_impl;
};

} // namespace coro

#else // !CORO_UDP_BACKEND_LWIP — libuv-backed implementation

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/io/byte_buffer.h>
#include <coro/io/socket_address.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/task/join_handle.h>
#include <coro/coro.h>
#include <uv.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace coro {

/**
 * @brief Async, connectionless UDP socket. Obtain via `co_await UdpSocket::bind()`.
 *
 * See doc/design/udp_socket.md. `recv_from()`/`recv()` first attempt a direct,
 * non-blocking read on the calling thread (no uv-thread hop); only on `EAGAIN`
 * do they hop to the uv thread and suspend. There is no internal receive queue
 * on this backend either — arriving datagrams are held by the kernel's own
 * per-socket receive buffer between calls, exactly as for any other UDP socket.
 *
 * **Concurrency:** only one receive (`recv_from()`/`recv()`) and only one send
 * (`send_to()`/`send()`) may be in flight at a time; `connect()` must not run
 * concurrently with either.
 */
class UdpSocket {
public:
    UdpSocket(UdpSocket&&) noexcept;
    UdpSocket& operator=(UdpSocket&&) noexcept;
    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// Closes the socket asynchronously on the uv executor. Does not block.
    ~UdpSocket();

    /// Binds a UDP socket to host:port. host must be dotted-decimal, an IPv6
    /// literal, or "0.0.0.0"/"::".
    [[nodiscard]] static JoinHandle<UdpSocket> bind(std::string host, uint16_t port);

    /// Sends buf as a single datagram to dest. Returns buf once the send completes.
    template<ByteBuffer Buf>
    [[nodiscard]] JoinHandle<Buf> send_to(Buf buf, SocketAddress dest);

    /// Waits for the next datagram, copying it into buf. Returns {n, buf, sender}.
    /// Oversized datagrams are truncated to fit buf, same as POSIX recvfrom().
    template<ByteBuffer Buf>
    [[nodiscard]] Coro<std::tuple<std::size_t, Buf, SocketAddress>> recv_from(Buf buf);

    /// Fixes peer as this socket's only correspondent. Once connected, plain
    /// send()/recv() may be used instead of send_to()/recv_from(); datagrams from
    /// any other address are dropped by the OS before they ever reach recv().
    [[nodiscard]] JoinHandle<void> connect(SocketAddress peer);

    /// Sends buf to the peer fixed by connect(). Throws (UV_EDESTADDRREQ) if not connected.
    template<ByteBuffer Buf>
    [[nodiscard]] JoinHandle<Buf> send(Buf buf);

    /// Waits for the next datagram from the peer fixed by connect(), copying it into buf.
    template<ByteBuffer Buf>
    [[nodiscard]] Coro<std::pair<std::size_t, Buf>> recv(Buf buf);

    /// Enables (or disables) sending to broadcast addresses via send_to()/send().
    /// Required before a sendto() to a broadcast address is permitted by the OS
    /// (SO_BROADCAST) — otherwise it fails with EACCES.
    [[nodiscard]] JoinHandle<void> set_broadcast(bool enabled);

    /// Joins multicast group so recv_from()/recv() start receiving datagrams sent
    /// to it. iface selects which local interface to join on; the default
    /// (Ipv4Address{}) lets the OS choose. IPv4 only.
    [[nodiscard]] JoinHandle<void> join_multicast(Ipv4Address group, Ipv4Address iface = {});

    /// Leaves a multicast group previously joined with join_multicast().
    [[nodiscard]] JoinHandle<void> leave_multicast(Ipv4Address group, Ipv4Address iface = {});

private:
    // Heap-allocated uv_udp_t — address must be stable across the handle lifetime.
    struct Handle {
        uv_udp_t handle;
        int      raw_fd = -1;   // cached via uv_fileno() in bind(); lets recv_from()
                                 // attempt a raw read without a uv-thread hop
    };

    explicit UdpSocket(std::shared_ptr<Handle> handle, SingleThreadedUvExecutor* uv_exec);

    // Shared fast-path-then-suspend implementation behind recv_from()/recv();
    // defined in udp_socket.hpp (template, since it's parameterized on Buf).
    template<ByteBuffer Buf>
    static Coro<std::tuple<std::size_t, Buf, SocketAddress>> recv_from_impl(
        std::shared_ptr<Handle> handle, SingleThreadedUvExecutor* uv_exec, Buf buf);

    // Shared by join_multicast()/leave_multicast(); defined in udp_socket.cpp.
    static JoinHandle<void> set_membership(std::shared_ptr<Handle> handle,
        SingleThreadedUvExecutor* uv_exec, Ipv4Address group, Ipv4Address iface,
        uv_membership membership);

    std::shared_ptr<Handle>   m_handle;
    SingleThreadedUvExecutor* m_uv_exec = nullptr;
};

} // namespace coro

#include <coro/io/udp_socket.hpp>

#endif // CORO_UDP_BACKEND_LWIP
