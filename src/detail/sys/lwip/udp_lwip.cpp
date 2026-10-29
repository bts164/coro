// lwIP backend for the UDP seam (include/coro/detail/sys/udp.h), over the raw UDP API.
// See doc/design/pico_port.md, "UDP" and "Received data: per-socket buffers".
//
// The receive callback stays installed for the socket's lifetime and copies each
// datagram into the socket's ring, so datagrams that arrive between receives wait
// there, as they wait in SO_RCVBUF on POSIX. Sends are synchronous in lwIP.

#include "io_object.h"

#include <coro/detail/sys/udp.h>

#include <lwip/igmp.h>
#include <lwip/netif.h>
#include <lwip/udp.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>

#ifndef CORO_LWIP_UDP_RX_BYTES
/// Bytes of received datagrams (with an 8-byte record header each) a UDP socket holds
/// for its reader before it drops new ones. See doc/design/lwip_config.md, "Receive
/// memory".
#define CORO_LWIP_UDP_RX_BYTES 8192
#endif

namespace coro::detail::sys {

namespace {

// Runs on the executor's thread inside Poller::poll(). Must free `p`.
void on_recv(void* arg, udp_pcb* /*pcb*/, pbuf* p, const ip_addr_t* addr, u16_t port) {
    auto* udp = static_cast<LwipUdp*>(arg);
    const std::size_t need = sizeof(LwipUdp::RecordHeader) + p->tot_len;
    if (udp->rx && need <= udp->rx->writable_bytes()) {
        LwipUdp::RecordHeader header{};
        header.length = p->tot_len;
        header.port   = port;
        const SocketAddress sender = from_ip_addr(*addr, port);
        const auto& v4 = std::get<Ipv4Address>(sender.address);
        std::copy(v4.octets.begin(), v4.octets.end(), header.addr);
        ring_write(*udp->rx, &header, sizeof(header));
        ring_write(*udp->rx, p);
        mark_ready(udp, IoObject::kReadable);
    }
    // Otherwise the ring is full: dropped, as a kernel drops at a full SO_RCVBUF.
    pbuf_free(p);
}

LwipUdp* as_udp(RawFd fd) { return static_cast<LwipUdp*>(fd); }

} // namespace

RawFd udp_open(const SocketAddress& local) {
    require_lwip("UdpSocket::bind");
    ip_addr_t addr;
    if (!to_ip_addr(local, addr)) throw_errno(EAFNOSUPPORT, "UdpSocket::bind");

    auto udp = std::make_unique<LwipUdp>();
    udp->rx.emplace(static_cast<std::size_t>(CORO_LWIP_UDP_RX_BYTES));
    udp_pcb* pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == nullptr) throw_errno(ENOMEM, "UdpSocket::bind");
    const err_t err = udp_bind(pcb, &addr, local.port);
    if (err != ERR_OK) {
        udp_remove(pcb);
        throw_errno(lwip_errno(err), "UdpSocket::bind");
    }
    udp->pcb = pcb;
    udp_recv(pcb, &on_recv, udp.get());
    return udp.release();
}

void udp_connect(RawFd fd, const SocketAddress& peer) {
    auto* udp = as_udp(fd);
    ip_addr_t addr;
    if (!to_ip_addr(peer, addr)) throw_errno(EAFNOSUPPORT, "UdpSocket::connect");
    // lwIP drops datagrams from any other sender from here on.
    const err_t err = ::udp_connect(udp->pcb, &addr, peer.port);
    if (err != ERR_OK) throw_errno(lwip_errno(err), "UdpSocket::connect");
    udp->connected = true;
}

void udp_set_broadcast(RawFd /*fd*/, bool /*enabled*/) {
    // lwIP checks an SO_BROADCAST-like pcb flag only with IP_SOF_BROADCAST, which
    // coro's lwipopts.h leaves at its default of 0: broadcast always works.
}

void udp_set_membership(RawFd /*fd*/, Ipv4Address group, Ipv4Address /*iface*/, bool join) {
#if LWIP_IGMP
    // A Pico has one interface, so `iface` is ignored.
    if (netif_default == nullptr) throw_errno(ENODEV, "UdpSocket::join_multicast");
    ip4_addr_t g;
    IP4_ADDR(&g, group.octets[0], group.octets[1], group.octets[2], group.octets[3]);
    const err_t err = join ? igmp_joingroup_netif(netif_default, &g)
                           : igmp_leavegroup_netif(netif_default, &g);
    if (err != ERR_OK)
        throw_errno(lwip_errno(err),
                    join ? "UdpSocket::join_multicast" : "UdpSocket::leave_multicast");
#else
    (void)group;
    throw_errno(ENOTSUP, join ? "UdpSocket::join_multicast" : "UdpSocket::leave_multicast");
#endif
}

void udp_set_segment_size(RawFd /*fd*/, std::size_t /*bytes*/) {
    throw_errno(ENOTSUP, "UdpSocket::set_segment_size");
}

void udp_set_gro(RawFd /*fd*/, bool /*enabled*/) {
    throw_errno(ENOTSUP, "UdpSocket::set_gro");
}

IoResult udp_try_send(RawFd fd, const std::byte* data, std::size_t size,
                      const SocketAddress* dest) noexcept {
    auto* udp = as_udp(fd);
    if (dest == nullptr && !udp->connected) return std::unexpected(EDESTADDRREQ);
    ip_addr_t addr{};
    if (dest != nullptr && !to_ip_addr(*dest, addr)) return std::unexpected(EAFNOSUPPORT);
    if (size > 0xFFFF) return std::unexpected(EMSGSIZE);
    pbuf* p = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(size), PBUF_RAM);
    // Never a wait: no callback would report the socket writable again.
    if (p == nullptr) return std::unexpected(ENOBUFS);
    pbuf_take(p, data, static_cast<u16_t>(size));
    const err_t err = dest != nullptr ? udp_sendto(udp->pcb, p, &addr, dest->port)
                                      : udp_send(udp->pcb, p);
    pbuf_free(p);
    if (err == ERR_MEM || err == ERR_BUF) return std::unexpected(ENOBUFS);
    if (err != ERR_OK) return std::unexpected(lwip_errno(err));
    return size;
}

IoResult udp_try_recv(RawFd fd, std::byte* data, std::size_t size,
                      SocketAddress* sender) noexcept {
    auto* udp = as_udp(fd);
    if (!udp->rx || udp->rx->readable_bytes() == 0) return std::unexpected(EAGAIN);
    LwipUdp::RecordHeader header{};
    ring_read(*udp->rx, &header, sizeof(header));
    // Truncated to the caller's buffer, as recvfrom() does; the rest is discarded.
    const std::size_t n = std::min<std::size_t>(size, header.length);
    ring_read(*udp->rx, data, n);
    ring_read(*udp->rx, nullptr, header.length - n);
    if (sender != nullptr) {
        Ipv4Address v4;
        std::copy(header.addr, header.addr + 4, v4.octets.begin());
        *sender = SocketAddress{v4, header.port};
    }
    return n;
}

IoResult udp_try_recv_segments(RawFd fd, std::byte* data, std::size_t size,
                               SocketAddress& sender, std::size_t& segment_size) noexcept {
    // No GRO on lwIP: one datagram per read.
    auto result = udp_try_recv(fd, data, size, &sender);
    if (result) segment_size = *result;
    return result;
}

void close_udp(LwipUdp* udp) noexcept {
    if (udp->pcb != nullptr) {
        udp_recv(udp->pcb, nullptr, nullptr);
        udp_remove(udp->pcb);
        udp->pcb = nullptr;
    }
    delete udp;
}

} // namespace coro::detail::sys
