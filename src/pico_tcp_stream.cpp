#include <coro/pico/pico_tcp_stream.h>
#include <coro/pico/pico_callback_result.h>
#include <lwip/dns.h>
#include <lwip/pbuf.h>
#include <stdexcept>
#include <utility>

namespace coro {

// ---------------------------------------------------------------------------
// PicoTcpContext callbacks
// ---------------------------------------------------------------------------

// Called by lwIP when data arrives or the connection is closed.
// p == nullptr signals EOF (remote closed the connection).
err_t PicoTcpContext::on_recv(void* arg, tcp_pcb* tpcb, pbuf* p, err_t err) {
    auto* ctx = static_cast<PicoTcpContext*>(arg);

    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        ctx->errored = true;
        ctx->error   = err;
        ctx->pcb     = nullptr;
        if (ctx->rx_waker) {
            auto w = std::move(ctx->rx_waker);
            w->wake();
        }
        return ERR_OK;
    }

    if (p == nullptr) {
        ctx->rx_eof = true;
        if (ctx->rx_waker) {
            auto w = std::move(ctx->rx_waker);
            w->wake();
        }
        return ERR_OK;
    }

    // Copy the pbuf chain into rx_buf. tcp_recved() tells lwIP we've consumed
    // `p->tot_len` bytes, allowing it to open the receive window and ACK.
    const auto old_size = ctx->rx_buf.size();
    ctx->rx_buf.resize(old_size + p->tot_len);
    pbuf_copy_partial(p, ctx->rx_buf.data() + old_size, p->tot_len, 0);
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    if (ctx->rx_waker) {
        auto w = std::move(ctx->rx_waker);
        w->wake();
    }
    return ERR_OK;
}

// Called by lwIP when previously queued data has been acknowledged by the remote.
// `len` is the number of bytes ACKed in this callback (may be less than the
// total outstanding). Wakes any write() suspended on send-buffer backpressure.
err_t PicoTcpContext::on_sent(void* arg, tcp_pcb* /*tpcb*/, uint16_t /*len*/) {
    auto* ctx = static_cast<PicoTcpContext*>(arg);
    if (ctx->tx_waker) {
        auto w = std::move(ctx->tx_waker);
        w->wake();
    }
    return ERR_OK;
}

// Called by lwIP on a fatal connection error. The PCB is already freed by lwIP
// before this callback returns, so we must never dereference ctx->pcb afterward.
void PicoTcpContext::on_err(void* arg, err_t err) {
    auto* ctx = static_cast<PicoTcpContext*>(arg);
    ctx->errored = true;
    ctx->error   = err;
    ctx->pcb     = nullptr;  // PCB freed by lwIP; never dereference after this point

    if (ctx->rx_waker) {
        auto w = std::move(ctx->rx_waker);
        w->wake();
    }
    if (ctx->tx_waker) {
        auto w = std::move(ctx->tx_waker);
        w->wake();
    }
}

// ---------------------------------------------------------------------------
// PicoTcpStream — construction / destruction
// ---------------------------------------------------------------------------

PicoTcpStream::PicoTcpStream(std::shared_ptr<PicoTcpContext> ctx)
    : m_ctx(std::move(ctx)) {}

PicoTcpStream::PicoTcpStream(PicoTcpStream&&) noexcept = default;
PicoTcpStream& PicoTcpStream::operator=(PicoTcpStream&&) noexcept = default;

PicoTcpStream::~PicoTcpStream() {
    if (!m_ctx || !m_ctx->pcb) return;
    // Clear all callbacks so they can't fire with a dangling ctx pointer after
    // m_ctx is freed. tcp_abort() sends RST and frees the PCB immediately.
    tcp_arg (m_ctx->pcb, nullptr);
    tcp_recv(m_ctx->pcb, nullptr);
    tcp_sent(m_ctx->pcb, nullptr);
    tcp_err (m_ctx->pcb, nullptr);
    tcp_abort(m_ctx->pcb);
    m_ctx->pcb = nullptr;
}

// ---------------------------------------------------------------------------
// PicoTcpStream::connect
// ---------------------------------------------------------------------------

Coro<PicoTcpStream> PicoTcpStream::connect(const char* host, uint16_t port) {
    // ---- Step 1: DNS resolution ----
    // dns_gethostbyname returns ERR_OK (address cached) or ERR_INPROGRESS (async).
    // The callback receives nullptr on failure.
    PicoCallbackResult<ip_addr_t, bool> dns_cb;
    ip_addr_t addr{};

    err_t dns_err = dns_gethostbyname(
        host, &addr,
        +[](const char*, const ip_addr_t* resolved, void* arg) {
            auto* cb = static_cast<PicoCallbackResult<ip_addr_t, bool>*>(arg);
            if (resolved) cb->complete(*resolved, true);
            else          cb->complete(ip_addr_t{}, false);
        },
        &dns_cb);

    if (dns_err == ERR_INPROGRESS) {
        auto [resolved, ok] = co_await pico_wait(dns_cb);
        if (!ok)
            throw std::runtime_error("PicoTcpStream::connect: DNS lookup failed");
        addr = resolved;
    } else if (dns_err != ERR_OK) {
        throw std::runtime_error("PicoTcpStream::connect: dns_gethostbyname failed");
    }
    // If dns_err == ERR_OK, addr was filled synchronously from the cache.

    // ---- Step 2: TCP connect ----
    // ConnState lives on this coroutine frame. The tcp_err and tcp_connect callbacks
    // both hold a raw pointer to it; the frame stays alive until co_await returns.
    //
    // lwIP fires tcp_err (not the connect callback) on connection failure; tcp_err
    // frees the PCB before returning, so we must not call tcp_abort after it fires.
    // `fired` guards against both callbacks running (should not happen in practice).
    struct ConnState {
        PicoCallbackResult<err_t> result;
        bool                      fired = false;
    } conn;

    tcp_pcb* pcb = tcp_new();
    if (!pcb)
        throw std::runtime_error("PicoTcpStream::connect: tcp_new failed");

    tcp_arg(pcb, &conn);

    tcp_err(pcb, +[](void* arg, err_t err) {
        auto* conn = static_cast<ConnState*>(arg);
        if (!conn->fired) {
            conn->fired = true;
            conn->result.complete(err);
        }
        // pcb is already freed by lwIP — do not touch it
    });

    err_t conn_err = tcp_connect(
        pcb, &addr, port,
        +[](void* arg, tcp_pcb*, err_t err) -> err_t {
            // lwIP only calls this callback with err == ERR_OK on success.
            auto* conn = static_cast<ConnState*>(arg);
            if (!conn->fired) {
                conn->fired = true;
                conn->result.complete(err);
            }
            return ERR_OK;
        });

    if (conn_err != ERR_OK) {
        tcp_abort(pcb);
        throw std::runtime_error("PicoTcpStream::connect: tcp_connect failed");
    }

    auto [status] = co_await pico_wait(conn.result);
    if (status != ERR_OK) {
        // PCB was already freed by lwIP inside the tcp_err callback — do not abort.
        throw std::runtime_error("PicoTcpStream::connect: connection failed");
    }

    // ---- Step 3: wire up the stream context ----
    auto ctx = std::make_shared<PicoTcpContext>();
    ctx->pcb = pcb;
    tcp_arg (pcb, ctx.get());
    tcp_recv(pcb, PicoTcpContext::on_recv);
    tcp_sent(pcb, PicoTcpContext::on_sent);
    tcp_err (pcb, PicoTcpContext::on_err);

    co_return PicoTcpStream(std::move(ctx));
}

} // namespace coro
