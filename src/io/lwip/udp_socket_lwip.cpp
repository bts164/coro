// lwIP-backed UdpSocket implementation for NO_SYS mode.
//
// Compile as part of the application (not the coro library):
//   target_sources(my_app PRIVATE ${CORO_ROOT}/src/io/lwip/udp_socket_lwip.cpp)
//   target_link_libraries(my_app PRIVATE coro::coro lwip)
//
// All lwIP callbacks fire synchronously on the executor thread during
// cyw43_arch_poll() (Pico) or sys_check_timeouts() (host test builds).

#include <coro/io/udp_socket.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include "lwip_udp_ctx.h"
#include <lwip/igmp.h>
#include <lwip/netif.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <variant>

// ---------------------------------------------------------------------------
// LwipUdpCtx::on_recv  (coro::detail namespace)
// ---------------------------------------------------------------------------

namespace coro::detail {

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
    ctx->result_sender = SocketAddress{v4, port};
    ctx->result_ready  = true;
    if (ctx->rx_waker) { auto w = std::move(ctx->rx_waker); w->wake(); }
}

} // namespace coro::detail

// ---------------------------------------------------------------------------
// UdpSocket methods  (coro namespace)
// ---------------------------------------------------------------------------

namespace coro {

UdpSocket::UdpSocket(detail::Rc<detail::LwipUdpCtx> impl) : m_impl(std::move(impl)) {}
UdpSocket::UdpSocket(UdpSocket&&) noexcept = default;
UdpSocket& UdpSocket::operator=(UdpSocket&&) noexcept = default;

UdpSocket::~UdpSocket() {
    if (!m_impl || !m_impl->pcb) return;
    udp_recv(m_impl->pcb, nullptr, nullptr);  // detach callback before removal
    udp_remove(m_impl->pcb);
    m_impl->pcb = nullptr;
}

// ---------------------------------------------------------------------------
// bind
// ---------------------------------------------------------------------------

Coro<UdpSocket> UdpSocket::bind(std::string host, uint16_t port) {
    auto v4 = SocketAddress::parse(host, port);
    if (!v4 || !std::holds_alternative<Ipv4Address>(v4->address))
        throw std::runtime_error("UdpSocket::bind: invalid IPv4 host");
    const auto& octets = std::get<Ipv4Address>(v4->address).octets;

    udp_pcb* pcb = udp_new();
    if (!pcb) throw std::runtime_error("UdpSocket::bind: udp_new failed");

    ip_addr_t addr;
    IP4_ADDR(&addr, octets[0], octets[1], octets[2], octets[3]);

    err_t err = udp_bind(pcb, &addr, port);
    if (err != ERR_OK) {
        udp_remove(pcb);
        throw std::runtime_error("UdpSocket::bind: udp_bind failed");
    }

    auto ctx = detail::make_rc<detail::LwipUdpCtx>();
    ctx->pcb = pcb;

    co_return UdpSocket(std::move(ctx));
}

// ---------------------------------------------------------------------------
// connect
// ---------------------------------------------------------------------------

Coro<void> UdpSocket::connect(SocketAddress peer) {
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

// ---------------------------------------------------------------------------
// send_to_impl / send_impl
// ---------------------------------------------------------------------------

Coro<void> UdpSocket::send_to_impl(const std::byte* buf, std::size_t size, SocketAddress dest) {
    if (!std::holds_alternative<Ipv4Address>(dest.address))
        throw std::runtime_error("UdpSocket::send_to: IPv6 destination not supported on the lwIP backend");
    const auto& v4 = std::get<Ipv4Address>(dest.address);

    pbuf* p = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(size), PBUF_RAM);
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

Coro<void> UdpSocket::send_impl(const std::byte* buf, std::size_t size) {
    if (!m_impl->connected)
        throw std::runtime_error("UdpSocket::send: not connected — call connect() first");

    pbuf* p = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(size), PBUF_RAM);
    if (!p) throw std::runtime_error("UdpSocket::send: pbuf_alloc failed (out of memory)");
    std::memcpy(p->payload, buf, size);

    err_t err = udp_send(m_impl->pcb, p);  // no addr/port — uses the connected peer
    pbuf_free(p);
    if (err != ERR_OK)
        throw std::runtime_error("UdpSocket::send: udp_send failed");
    co_return;
}

// ---------------------------------------------------------------------------
// recv_from_impl / recv_impl
// ---------------------------------------------------------------------------

Coro<std::tuple<std::size_t, SocketAddress>> UdpSocket::recv_from_impl(std::byte* buf, std::size_t size) {
    auto ctx_ptr = m_impl;

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

    ctx_ptr->pending_buf  = buf;
    ctx_ptr->pending_len  = size;
    ctx_ptr->result_ready = false;
    udp_recv(ctx_ptr->pcb, &detail::LwipUdpCtx::on_recv, ctx_ptr.get());

    co_await DatagramReady{ctx_ptr};

    co_return std::tuple<std::size_t, SocketAddress>{ctx_ptr->result_len, ctx_ptr->result_sender};
}

Coro<std::size_t> UdpSocket::recv_impl(std::byte* buf, std::size_t size) {
    if (!m_impl->connected)
        throw std::runtime_error("UdpSocket::recv: not connected — call connect() first");
    auto [n, sender] = co_await recv_from_impl(buf, size);
    (void)sender; // connected socket — lwIP already filtered to only our peer
    co_return n;
}

// ---------------------------------------------------------------------------
// set_broadcast_impl, join_multicast_impl, leave_multicast_impl
// ---------------------------------------------------------------------------

Coro<void> UdpSocket::set_broadcast(bool enabled) {
    // No-op: IP_SOF_BROADCAST / IP_SOF_BROADCAST_RECV both default to 0 (lwIP's own
    // opt.h default, left unset in this project's lwipopts.h.in), so udp_sendto_if()
    // never checks an SOF_BROADCAST pcb flag in the first place — see
    // doc/design/udp_socket.md's "Multicast and broadcast" section. Kept only for
    // API symmetry with the libuv backend.
    (void)enabled;
    co_return;
}

Coro<void> UdpSocket::join_multicast(Ipv4Address group, Ipv4Address iface) {
    (void)iface;  // Pico has exactly one network interface; always joins on netif_default
    ip4_addr_t addr;
    IP4_ADDR(&addr, group.octets[0], group.octets[1], group.octets[2], group.octets[3]);
    err_t err = igmp_joingroup_netif(netif_default, &addr);
    if (err != ERR_OK)
        throw std::runtime_error("UdpSocket::join_multicast: igmp_joingroup_netif failed");
    co_return;
}

Coro<void> UdpSocket::leave_multicast(Ipv4Address group, Ipv4Address iface) {
    (void)iface;
    ip4_addr_t addr;
    IP4_ADDR(&addr, group.octets[0], group.octets[1], group.octets[2], group.octets[3]);
    err_t err = igmp_leavegroup_netif(netif_default, &addr);
    if (err != ERR_OK)
        throw std::runtime_error("UdpSocket::leave_multicast: igmp_leavegroup_netif failed");
    co_return;
}

} // namespace coro
