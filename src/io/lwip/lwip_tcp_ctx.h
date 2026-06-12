#pragma once

// Internal lwIP TCP context — shared between tcp_stream_lwip.cpp and
// tcp_listener_lwip.cpp. Not part of the public API; never include from
// any header under include/coro/.

#include <coro/detail/waker.h>
#include <lwip/tcp.h>
#include <lwip/pbuf.h>
#include <memory>
#include <vector>

namespace coro::detail {

struct LwipTcpCtx {
    tcp_pcb* pcb = nullptr;

    // Receive
    std::vector<uint8_t>           rx_buf;
    std::shared_ptr<Waker> rx_waker;
    bool                           rx_eof  = false;

    // Transmit
    std::shared_ptr<Waker> tx_waker;

    // Fatal error (pcb is null after on_err fires — lwIP frees it first)
    bool  errored = false;
    err_t error   = ERR_OK;

    // lwIP static callbacks
    static err_t on_recv(void* arg, tcp_pcb* tpcb, pbuf* p, err_t err);
    static err_t on_sent(void* arg, tcp_pcb* tpcb, uint16_t len);
    static void  on_err (void* arg, err_t err);
};

} // namespace coro::detail
