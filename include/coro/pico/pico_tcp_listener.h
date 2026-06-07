#pragma once
#ifndef CORO_PICO
#error "pico_tcp_listener.h requires CORO_PICO to be defined. This header is only for Raspberry Pi Pico builds."
#endif

// Async TCP server for Raspberry Pi Pico W, backed by the lwIP raw TCP API.
//
// Requires: pico_cyw43_arch_lwip_poll and PicoExecutor.
// All accept callbacks fire synchronously from cyw43_arch_poll().

#include <coro/coro.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/pico/pico_tcp_stream.h>
#include <lwip/tcp.h>
#include <cstdint>
#include <deque>
#include <memory>

namespace coro {

// ---------------------------------------------------------------------------
// PicoListenContext — internal shared state between PicoTcpListener and lwIP
// ---------------------------------------------------------------------------

/**
 * @brief Internal state shared between a `PicoTcpListener` and its lwIP accept callback.
 *
 * `on_accept` is called by lwIP whenever a new connection completes the TCP
 * handshake. It pushes the new `tcp_pcb*` onto `pending` and wakes any
 * suspended `accept()` future. The `accept()` future pops from `pending` and
 * wraps each PCB in a `PicoTcpStream`.
 *
 * The `closed` flag is set before `~PicoTcpListener` closes the listen PCB, so
 * that any `on_accept` callback that fires in the same `cyw43_arch_poll()` call
 * immediately aborts the new PCB rather than queuing it for a dead listener.
 */
struct PicoListenContext {
    tcp_pcb* listen_pcb = nullptr;  // INVALID (null) after close

    std::deque<tcp_pcb*>           pending;       // accepted PCBs not yet consumed
    std::shared_ptr<detail::Waker> accept_waker;  // non-null while accept() is suspended
    bool                           closed = false;

    static err_t on_accept(void* arg, tcp_pcb* newpcb, err_t err);
};

// ---------------------------------------------------------------------------
// PicoTcpListener
// ---------------------------------------------------------------------------

/**
 * @brief Async TCP server for Raspberry Pi Pico W.
 *
 * Obtain via `co_await PicoTcpListener::bind(host, port)`. Each call to
 * `accept()` waits for the next incoming connection and returns a
 * `PicoTcpStream` ready for I/O.
 *
 * **Concurrency:** only one `accept()` may be in flight at a time. The
 * listener uses a single `accept_waker` slot; calling `accept()` from two
 * concurrent tasks would silently overwrite the waker.
 *
 * **Backpressure:** up to four connections can queue in `pending` before the
 * lwIP stack starts rejecting new SYNs. Increase `TCP_LISTEN_BACKLOG` in
 * `lwipopts.h` if you need a larger queue.
 *
 * **Destruction:** destroying the listener aborts any queued pending
 * connections and wakes a suspended `accept()` with a "closed" error.
 */
class PicoTcpListener {
public:
    PicoTcpListener(PicoTcpListener&&) noexcept;
    PicoTcpListener& operator=(PicoTcpListener&&) noexcept;
    PicoTcpListener(const PicoTcpListener&)            = delete;
    PicoTcpListener& operator=(const PicoTcpListener&) = delete;

    /// Closes the listening socket and aborts any queued pending connections.
    ~PicoTcpListener();

    /**
     * @brief Binds to `host:port` and starts listening.
     *
     * `host` must be a dotted-decimal IPv4 address string or `"0.0.0.0"` to
     * listen on all interfaces. Hostname strings are not supported (no DNS
     * needed for a server bind).
     *
     * @throws std::runtime_error on bind or listen failure.
     */
    [[nodiscard]] static Coro<PicoTcpListener> bind(const char* host, uint16_t port);

    /**
     * @brief Waits for the next incoming connection.
     *
     * Suspends until a client completes the TCP handshake, then returns a
     * fully connected `PicoTcpStream`.
     *
     * @throws std::runtime_error if the listener has been closed.
     */
    [[nodiscard]] Coro<PicoTcpStream> accept();

private:
    explicit PicoTcpListener(std::shared_ptr<PicoListenContext> ctx);

    std::shared_ptr<PicoListenContext> m_ctx;
};

} // namespace coro
