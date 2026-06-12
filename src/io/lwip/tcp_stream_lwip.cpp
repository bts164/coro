// lwIP-backed TcpStream implementation for NO_SYS mode.
//
// Compile as part of the application (not the coro library):
//   target_sources(my_app PRIVATE ${CORO_ROOT}/src/io/lwip/tcp_stream_lwip.cpp)
//   target_link_libraries(my_app PRIVATE coro::coro lwip)
//
// All lwIP callbacks fire synchronously on the executor thread during
// cyw43_arch_poll() (Pico) or sys_check_timeouts() (host test builds).

#include <coro/io/tcp_stream.h>
#include <coro/io/lwip/lwip_callback_result.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include "lwip_tcp_ctx.h"
#include <lwip/dns.h>
#include <lwip/err.h>  // err_t, ERR_* constants
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

// Returns a human-readable description of an lwIP error code.
// Names the common TCP errors explicitly; falls back to the numeric value
// for anything else (cross-reference lwip/err.h).
static std::string lwip_err_string(err_t err) {
    switch (err) {
        case ERR_OK:       return "ERR_OK";
        case ERR_MEM:      return "ERR_MEM (out of memory)";
        case ERR_TIMEOUT:  return "ERR_TIMEOUT";
        case ERR_RST:      return "ERR_RST (connection reset)";
        case ERR_ABRT:     return "ERR_ABRT (connection aborted)";
        case ERR_CLSD:     return "ERR_CLSD (connection closed)";
        case ERR_CONN:     return "ERR_CONN (not connected)";
        case ERR_RTE:      return "ERR_RTE (routing error)";
        case ERR_IF:       return "ERR_IF (netif error)";
        default:           return "lwip err=" + std::to_string(static_cast<int>(err));
    }
}

// ---------------------------------------------------------------------------
// LwipTcpCtx callbacks  (coro::detail namespace)
// ---------------------------------------------------------------------------

namespace coro::detail {

err_t LwipTcpCtx::on_recv(void* arg, tcp_pcb* tpcb, pbuf* p, err_t err) {
    auto* ctx = static_cast<LwipTcpCtx*>(arg);

    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        ctx->errored = true;
        ctx->error   = err;
        ctx->pcb     = nullptr;
        if (ctx->rx_waker) { auto w = std::move(ctx->rx_waker); w->wake(); }
        return ERR_OK;
    }

    if (p == nullptr) {
        // EOF — remote closed the connection
        ctx->rx_eof = true;
        if (ctx->rx_waker) { auto w = std::move(ctx->rx_waker); w->wake(); }
        return ERR_OK;
    }

    const auto old_size = ctx->rx_buf.size();
    ctx->rx_buf.resize(old_size + p->tot_len);
    pbuf_copy_partial(p, ctx->rx_buf.data() + old_size, p->tot_len, 0);
    // Inform lwIP we've consumed the data so it can open the receive window.
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    if (ctx->rx_waker) { auto w = std::move(ctx->rx_waker); w->wake(); }
    return ERR_OK;
}

err_t LwipTcpCtx::on_sent(void* arg, tcp_pcb* /*tpcb*/, uint16_t /*len*/) {
    auto* ctx = static_cast<LwipTcpCtx*>(arg);
    if (ctx->tx_waker) { auto w = std::move(ctx->tx_waker); w->wake(); }
    return ERR_OK;
}

// Called by lwIP after a fatal error. The PCB is already freed before this
// callback returns, so pcb must be set to nullptr to prevent any further use.
void LwipTcpCtx::on_err(void* arg, err_t err) {
    auto* ctx = static_cast<LwipTcpCtx*>(arg);
    ctx->errored = true;
    ctx->error   = err;
    ctx->pcb     = nullptr;  // RACE CONDITION NOTE: safe — callbacks fire on the executor thread

    if (ctx->rx_waker) { auto w = std::move(ctx->rx_waker); w->wake(); }
    if (ctx->tx_waker) { auto w = std::move(ctx->tx_waker); w->wake(); }
}

} // namespace coro::detail

// ---------------------------------------------------------------------------
// TcpStream methods  (coro namespace)
// ---------------------------------------------------------------------------

namespace coro {

TcpStream::TcpStream(std::shared_ptr<detail::LwipTcpCtx> impl)
    : m_impl(std::move(impl)) {}
TcpStream::TcpStream(TcpStream&&) noexcept = default;
TcpStream& TcpStream::operator=(TcpStream&&) noexcept = default;

TcpStream::~TcpStream() {
    if (!m_impl || !m_impl->pcb) return;
    // Clear callbacks so they cannot fire with a dangling ctx pointer after
    // m_impl is freed. lwIP manages PCB lifetime after tcp_close().
    tcp_arg (m_impl->pcb, nullptr);
    tcp_recv(m_impl->pcb, nullptr);
    tcp_sent(m_impl->pcb, nullptr);
    tcp_err (m_impl->pcb, nullptr);
    // Use graceful close (FIN) so any data already written with tcp_write() /
    // tcp_output() is transmitted before the connection tears down. tcp_abort()
    // sends RST immediately and races with in-flight data — the peer discards
    // buffered data on RST. Fall back to abort only if close fails (OOM).
    if (tcp_close(m_impl->pcb) != ERR_OK)
        tcp_abort(m_impl->pcb);
    m_impl->pcb = nullptr;
}

// ---------------------------------------------------------------------------
// TcpStream::connect
// ---------------------------------------------------------------------------

Coro<TcpStream> TcpStream::connect(std::string host, uint16_t port) {
    // ---- Step 1: DNS resolution ----
    LwipCallbackResult<ip_addr_t, bool> dns_cb;
    ip_addr_t addr{};

    // host is on the coroutine frame — valid until connect() completes.
    err_t dns_err = dns_gethostbyname(
        host.c_str(), &addr,
        +[](const char*, const ip_addr_t* resolved, void* arg) {
            auto* cb = static_cast<LwipCallbackResult<ip_addr_t, bool>*>(arg);
            if (resolved) cb->complete(*resolved, true);
            else          cb->complete(ip_addr_t{}, false);
        },
        &dns_cb);

    if (dns_err == ERR_INPROGRESS) {
        auto [resolved, ok] = co_await lwip_wait(dns_cb);
        if (!ok) throw std::runtime_error("TcpStream::connect: DNS lookup failed");
        addr = resolved;
    } else if (dns_err != ERR_OK) {
        throw std::runtime_error("TcpStream::connect: dns_gethostbyname failed");
    }

    // ---- Step 2: TCP connect ----
    // ConnState lives on this coroutine frame. Both callbacks hold a raw pointer
    // to it; the frame stays alive until co_await returns.
    // `fired` guards against both callbacks firing (should not happen in practice).
    struct ConnState {
        LwipCallbackResult<err_t> result;
        bool                      fired = false;
    } conn;

    tcp_pcb* pcb = tcp_new();
    if (!pcb) throw std::runtime_error("TcpStream::connect: tcp_new failed");

    tcp_arg(pcb, &conn);
    tcp_err(pcb, +[](void* arg, err_t err) {
        // lwIP fires tcp_err on failure and frees the PCB before this callback
        // returns — never touch pcb after this point.
        auto* c = static_cast<ConnState*>(arg);
        if (!c->fired) { c->fired = true; c->result.complete(err); }
    });

    err_t conn_err = tcp_connect(pcb, &addr, port,
        +[](void* arg, tcp_pcb*, err_t err) -> err_t {
            auto* c = static_cast<ConnState*>(arg);
            if (!c->fired) { c->fired = true; c->result.complete(err); }
            return ERR_OK;
        });

    if (conn_err != ERR_OK) {
        tcp_abort(pcb);
        throw std::runtime_error("TcpStream::connect: tcp_connect failed");
    }

    auto [status] = co_await lwip_wait(conn.result);
    if (status != ERR_OK)
        throw std::runtime_error("TcpStream::connect: " + lwip_err_string(status));

    // ---- Step 3: wire up the stream context ----
    auto ctx = std::make_shared<detail::LwipTcpCtx>();
    ctx->pcb = pcb;
    // Disable Nagle: send all data immediately without waiting for ACKs to
    // batch small segments. Required for correctness when the stream is
    // destroyed shortly after the last write — Nagle would otherwise hold
    // back the final sub-MSS segment until ACKs arrive, and tcp_abort() in
    // the destructor would discard it before it is ever sent.
    tcp_nagle_disable(pcb);
    tcp_arg (pcb, ctx.get());
    tcp_recv(pcb, detail::LwipTcpCtx::on_recv);
    tcp_sent(pcb, detail::LwipTcpCtx::on_sent);
    tcp_err (pcb, detail::LwipTcpCtx::on_err);

    co_return TcpStream(std::move(ctx));
}

// ---------------------------------------------------------------------------
// TcpStream::read_impl
// ---------------------------------------------------------------------------

Coro<std::size_t> TcpStream::read_impl(std::byte* buf, std::size_t size) {
    // Capture ctx before any suspension so we don't hold a dangling `this`
    // if the TcpStream object is moved while this coroutine is parked.
    auto ctx_ptr = m_impl;

    struct DataReady {
        using OutputType = void;
        std::shared_ptr<detail::LwipTcpCtx> ctx;

        PollResult<void> poll(detail::Context& cx) {
            if (!ctx->rx_buf.empty() || ctx->rx_eof || ctx->errored)
                return PollReady;
            // RACE CONDITION NOTE: safe — on_recv fires on the executor thread
            // (from cyw43_arch_poll / sys_check_timeouts), never concurrently.
            ctx->rx_waker = cx.getWaker();
            return PollPending;
        }
    };

    if (ctx_ptr->rx_buf.empty() && !ctx_ptr->rx_eof && !ctx_ptr->errored)
        co_await DataReady{ctx_ptr};

    // Drain buffered data first, even if the connection has since errored or
    // closed. A RST or FIN can arrive immediately after the last data segment
    // (common on loopback); reporting the error before returning buffered bytes
    // would discard valid data the peer already sent. The error or EOF is only
    // surfaced on the next call once the buffer is empty.
    if (!ctx_ptr->rx_buf.empty()) {
        auto n = std::min(size, ctx_ptr->rx_buf.size());
        std::memcpy(buf, ctx_ptr->rx_buf.data(), n);
        ctx_ptr->rx_buf.erase(ctx_ptr->rx_buf.begin(),
                              ctx_ptr->rx_buf.begin() + static_cast<std::ptrdiff_t>(n));
        co_return n;
    }

    if (ctx_ptr->errored)
        throw std::runtime_error("TcpStream::read: " + lwip_err_string(ctx_ptr->error));

    co_return std::size_t{0}; // EOF — graceful close, buffer already empty
}

// ---------------------------------------------------------------------------
// TcpStream::write_impl
// ---------------------------------------------------------------------------

Coro<void> TcpStream::write_impl(const std::byte* buf, std::size_t size) {
    auto ctx_ptr = m_impl;

    struct SpaceAvailable {
        using OutputType = void;
        std::shared_ptr<detail::LwipTcpCtx> ctx;

        PollResult<void> poll(detail::Context& cx) {
            if (ctx->errored || !ctx->pcb) return PollReady;
            if (tcp_sndbuf(ctx->pcb) > 0)  return PollReady;
            ctx->tx_waker = cx.getWaker();
            return PollPending;
        }
    };

    if (ctx_ptr->errored || !ctx_ptr->pcb)
        throw std::runtime_error("TcpStream::write: " + lwip_err_string(ctx_ptr->error));

    const auto* src = reinterpret_cast<const uint8_t*>(buf);
    std::size_t offset = 0;

    while (offset < size) {
        if (ctx_ptr->errored || !ctx_ptr->pcb)
            throw std::runtime_error("TcpStream::write: " + lwip_err_string(ctx_ptr->error));

        uint16_t avail = tcp_sndbuf(ctx_ptr->pcb);
        if (avail == 0) {
            tcp_output(ctx_ptr->pcb);
            co_await SpaceAvailable{ctx_ptr};
            continue;
        }

        uint16_t chunk = static_cast<uint16_t>(
            std::min<std::size_t>(avail, size - offset));
        // TCP_WRITE_FLAG_COPY: lwIP copies the data immediately, so src only
        // needs to be valid until tcp_write returns.
        err_t err = tcp_write(ctx_ptr->pcb, src + offset, chunk, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK)
            throw std::runtime_error("TcpStream::write: tcp_write failed");
        offset += chunk;
    }

    if (ctx_ptr->pcb)
        tcp_output(ctx_ptr->pcb);
}

} // namespace coro
