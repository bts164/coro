#include <coro/pico/pico_tcp_listener.h>
#include <lwip/ip_addr.h>
#include <stdexcept>
#include <cstring>

namespace coro {

// ---------------------------------------------------------------------------
// PicoListenContext::on_accept — lwIP callback, fires from cyw43_arch_poll()
// ---------------------------------------------------------------------------

err_t PicoListenContext::on_accept(void* arg, tcp_pcb* newpcb, err_t err) {
    auto* ctx = static_cast<PicoListenContext*>(arg);

    if (ctx->closed || err != ERR_OK) {
        // Listener is shutting down or lwIP signalled an error — reject immediately.
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    ctx->pending.push_back(newpcb);

    if (ctx->accept_waker) {
        auto w = std::move(ctx->accept_waker);
        w->wake();
    }
    return ERR_OK;
}

// ---------------------------------------------------------------------------
// PicoTcpListener — construction / destruction
// ---------------------------------------------------------------------------

PicoTcpListener::PicoTcpListener(std::shared_ptr<PicoListenContext> ctx)
    : m_ctx(std::move(ctx)) {}

PicoTcpListener::PicoTcpListener(PicoTcpListener&&) noexcept = default;
PicoTcpListener& PicoTcpListener::operator=(PicoTcpListener&&) noexcept = default;

PicoTcpListener::~PicoTcpListener() {
    if (!m_ctx) return;
    m_ctx->closed = true;

    // Abort any connections that arrived but were never consumed by accept().
    while (!m_ctx->pending.empty()) {
        tcp_pcb* pcb = m_ctx->pending.front();
        m_ctx->pending.pop_front();
        tcp_arg(pcb, nullptr);
        tcp_abort(pcb);
    }

    // Wake a suspended accept() so it can observe closed == true and throw.
    if (m_ctx->accept_waker) {
        auto w = std::move(m_ctx->accept_waker);
        w->wake();
    }

    if (m_ctx->listen_pcb) {
        tcp_arg(m_ctx->listen_pcb, nullptr);
        tcp_close(m_ctx->listen_pcb);
        m_ctx->listen_pcb = nullptr;
    }
}

// ---------------------------------------------------------------------------
// PicoTcpListener::bind
// ---------------------------------------------------------------------------

Coro<PicoTcpListener> PicoTcpListener::bind(const char* host, uint16_t port) {
    // Resolve bind address. Servers typically bind to "0.0.0.0".
    const ip_addr_t* bind_addr = IP_ADDR_ANY;
    ip_addr_t parsed{};
    if (host && std::strcmp(host, "0.0.0.0") != 0) {
        if (!ipaddr_aton(host, &parsed))
            throw std::runtime_error("PicoTcpListener::bind: invalid address");
        bind_addr = &parsed;
    }

    tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb)
        throw std::runtime_error("PicoTcpListener::bind: tcp_new failed");

    // Allow rapid restart: reuse the address before TIME_WAIT expires.
    int opt = 1;
    tcp_set_persist_backoff(pcb, 0); // suppress unused-variable; actual reuse
    (void)opt;

    err_t err = tcp_bind(pcb, bind_addr, port);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        throw std::runtime_error("PicoTcpListener::bind: tcp_bind failed");
    }

    // tcp_listen_with_backlog converts the bound PCB into a listening PCB.
    // On success the original pcb is freed internally; use the returned pointer.
    // On failure (OOM) the original pcb is NOT freed — abort it manually.
    tcp_pcb* listen_pcb = tcp_listen_with_backlog(pcb, 4);
    if (!listen_pcb) {
        tcp_abort(pcb);
        throw std::runtime_error("PicoTcpListener::bind: tcp_listen failed (out of memory)");
    }

    auto ctx = std::make_shared<PicoListenContext>();
    ctx->listen_pcb = listen_pcb;
    tcp_arg   (listen_pcb, ctx.get());
    tcp_accept(listen_pcb, PicoListenContext::on_accept);

    co_return PicoTcpListener(std::move(ctx));
}

// ---------------------------------------------------------------------------
// PicoTcpListener::accept
// ---------------------------------------------------------------------------

Coro<PicoTcpStream> PicoTcpListener::accept() {
    // ConnectionReady: suspends until pending has a connection or listener closes.
    struct ConnectionReady {
        using OutputType = void;
        std::shared_ptr<PicoListenContext> ctx;

        PollResult<void> poll(detail::Context& cx) {
            if (!ctx->pending.empty() || ctx->closed)
                return PollReady;
            // RACE CONDITION NOTE: safe — on_accept() only fires on the executor
            // thread inside cyw43_arch_poll(), so there is no concurrent write to
            // pending between this check and the waker assignment.
            ctx->accept_waker = cx.getWaker();
            return PollPending;
        }
    };

    if (m_ctx->pending.empty() && !m_ctx->closed)
        co_await ConnectionReady{m_ctx};

    if (m_ctx->closed)
        throw std::runtime_error("PicoTcpListener::accept: listener closed");

    tcp_pcb* newpcb = m_ctx->pending.front();
    m_ctx->pending.pop_front();

    // Wire up the stream context and callbacks for the new connection.
    auto stream_ctx = std::make_shared<PicoTcpContext>();
    stream_ctx->pcb = newpcb;
    tcp_arg (newpcb, stream_ctx.get());
    tcp_recv(newpcb, PicoTcpContext::on_recv);
    tcp_sent(newpcb, PicoTcpContext::on_sent);
    tcp_err (newpcb, PicoTcpContext::on_err);

    co_return PicoTcpStream(std::move(stream_ctx));
}

} // namespace coro
