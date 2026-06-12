// lwIP-backed TcpListener implementation for NO_SYS mode.
//
// Compile alongside tcp_stream_lwip.cpp:
//   target_sources(my_app PRIVATE
//       ${CORO_ROOT}/src/io/lwip/tcp_stream_lwip.cpp
//       ${CORO_ROOT}/src/io/lwip/tcp_listener_lwip.cpp)

#include <coro/io/tcp_listener.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include "lwip_tcp_ctx.h"
#include <lwip/ip_addr.h>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>

// ---------------------------------------------------------------------------
// LwipListenCtx  (coro::detail namespace)
// ---------------------------------------------------------------------------

namespace coro::detail {

struct LwipListenCtx {
    tcp_pcb* listen_pcb = nullptr;

    // Each accepted connection is fully wired up in on_accept so that data
    // arriving before accept() runs is buffered rather than dropped by lwIP.
    std::deque<std::shared_ptr<LwipTcpCtx>> pending;
    std::shared_ptr<Waker>                  accept_waker;
    bool                                    closed = false;

    static err_t on_accept(void* arg, tcp_pcb* newpcb, err_t err);
};

err_t LwipListenCtx::on_accept(void* arg, tcp_pcb* newpcb, err_t err) {
    auto* ctx = static_cast<LwipListenCtx*>(arg);

    if (ctx->closed || err != ERR_OK) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    // Wire up callbacks immediately so any data that arrives before accept()
    // runs gets buffered in LwipTcpCtx::rx_buf rather than dropped by lwIP.
    auto stream_ctx = std::make_shared<LwipTcpCtx>();
    stream_ctx->pcb = newpcb;
    tcp_nagle_disable(newpcb);
    tcp_arg (newpcb, stream_ctx.get());
    tcp_recv(newpcb, LwipTcpCtx::on_recv);
    tcp_sent(newpcb, LwipTcpCtx::on_sent);
    tcp_err (newpcb, LwipTcpCtx::on_err);

    ctx->pending.push_back(std::move(stream_ctx));
    if (ctx->accept_waker) { auto w = std::move(ctx->accept_waker); w->wake(); }
    return ERR_OK;
}

} // namespace coro::detail

// ---------------------------------------------------------------------------
// TcpListener methods  (coro namespace)
// ---------------------------------------------------------------------------

namespace coro {

TcpListener::TcpListener(std::shared_ptr<detail::LwipListenCtx> impl)
    : m_impl(std::move(impl)) {}
TcpListener::TcpListener(TcpListener&&) noexcept = default;
TcpListener& TcpListener::operator=(TcpListener&&) noexcept = default;

TcpListener::~TcpListener() {
    if (!m_impl) return;
    m_impl->closed = true;

    while (!m_impl->pending.empty()) {
        auto stream_ctx = std::move(m_impl->pending.front());
        m_impl->pending.pop_front();
        if (stream_ctx->pcb) {
            tcp_arg (stream_ctx->pcb, nullptr);
            tcp_recv(stream_ctx->pcb, nullptr);
            tcp_sent(stream_ctx->pcb, nullptr);
            tcp_err (stream_ctx->pcb, nullptr);
            tcp_abort(stream_ctx->pcb);
            stream_ctx->pcb = nullptr;
        }
    }

    if (m_impl->accept_waker) { auto w = std::move(m_impl->accept_waker); w->wake(); }

    if (m_impl->listen_pcb) {
        tcp_arg(m_impl->listen_pcb, nullptr);
        tcp_close(m_impl->listen_pcb);
        m_impl->listen_pcb = nullptr;
    }
}

// ---------------------------------------------------------------------------
// TcpListener::bind
// ---------------------------------------------------------------------------

Coro<TcpListener> TcpListener::bind(std::string host, uint16_t port) {
    const ip_addr_t* bind_addr = IP_ADDR_ANY;
    ip_addr_t parsed{};
    if (!host.empty() && host != "0.0.0.0") {
        if (!ipaddr_aton(host.c_str(), &parsed))
            throw std::runtime_error("TcpListener::bind: invalid address");
        bind_addr = &parsed;
    }

    tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) throw std::runtime_error("TcpListener::bind: tcp_new failed");

    err_t err = tcp_bind(pcb, bind_addr, port);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        throw std::runtime_error("TcpListener::bind: tcp_bind failed");
    }

    // tcp_listen_with_backlog converts the bound PCB into a listening PCB.
    // On success the original pcb is freed internally — use the returned pointer.
    // On failure the original pcb is NOT freed — abort it manually.
    tcp_pcb* listen_pcb = tcp_listen_with_backlog(pcb, 4);
    if (!listen_pcb) {
        tcp_abort(pcb);
        throw std::runtime_error("TcpListener::bind: tcp_listen failed (out of memory)");
    }

    auto ctx = std::make_shared<detail::LwipListenCtx>();
    ctx->listen_pcb = listen_pcb;
    tcp_arg   (listen_pcb, ctx.get());
    tcp_accept(listen_pcb, detail::LwipListenCtx::on_accept);

    co_return TcpListener(std::move(ctx));
}

// ---------------------------------------------------------------------------
// TcpListener::accept
// ---------------------------------------------------------------------------

Coro<TcpStream> TcpListener::accept() {
    // Capture ctx before any suspension for the same reason as read_impl.
    auto ctx_ptr = m_impl;

    struct ConnectionReady {
        using OutputType = void;
        std::shared_ptr<detail::LwipListenCtx> ctx;

        PollResult<void> poll(detail::Context& cx) {
            if (!ctx->pending.empty() || ctx->closed) return PollReady;
            // RACE CONDITION NOTE: safe — on_accept fires on the executor thread only.
            ctx->accept_waker = cx.getWaker();
            return PollPending;
        }
    };

    if (ctx_ptr->pending.empty() && !ctx_ptr->closed)
        co_await ConnectionReady{ctx_ptr};

    if (ctx_ptr->closed)
        throw std::runtime_error("TcpListener::accept: listener closed");

    // ctx was fully wired up in on_accept — just take ownership.
    auto stream_ctx = std::move(ctx_ptr->pending.front());
    ctx_ptr->pending.pop_front();

    co_return TcpStream(std::move(stream_ctx));
}

} // namespace coro
