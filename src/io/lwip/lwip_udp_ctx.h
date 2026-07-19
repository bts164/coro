#pragma once

// Internal lwIP UDP context — used by udp_socket_lwip.cpp. Not part of the
// public API; never include from any header under include/coro/.

#include <coro/detail/waker.h>
#include <coro/detail/rc.h>
#include <coro/io/socket_address.h>
#include <lwip/udp.h>
#include <lwip/pbuf.h>
#include <cstddef>

namespace coro::detail {

struct LwipUdpCtx {
    udp_pcb* pcb = nullptr;
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
