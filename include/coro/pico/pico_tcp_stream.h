#pragma once
#ifndef CORO_PICO
#error "pico_tcp_stream.h requires CORO_PICO to be defined. This header is only for Raspberry Pi Pico builds."
#endif

// Async TCP stream for Raspberry Pi Pico W, backed by the lwIP raw TCP API.
//
// Requires: pico_cyw43_arch_lwip_poll (or equivalent) and PicoExecutor.
// All operations are single-threaded; callbacks fire from cyw43_arch_poll().

#include <coro/coro.h>
#include <coro/io/byte_buffer.h>
#include <coro/detail/waker.h>
#include <lwip/tcp.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coro {

// ---------------------------------------------------------------------------
// PicoTcpContext — internal shared state between PicoTcpStream and lwIP callbacks
// ---------------------------------------------------------------------------

/**
 * @brief Internal state shared between a `PicoTcpStream` and its lwIP callbacks.
 *
 * Held by `std::shared_ptr` so that it outlives any in-flight `read()` or `write()`
 * future even if the stream object itself is moved or goes out of scope.
 *
 * ### Receive model
 * `on_recv` copies incoming pbuf data into `rx_buf` and wakes `rx_waker` if a
 * `read()` is suspended. Multiple `on_recv` calls accumulate in `rx_buf`; the
 * `read()` future drains up to `buf.size()` bytes per call.
 *
 * ### Transmit model
 * `write()` calls `tcp_write` (which copies the data internally) and `tcp_output`
 * in chunks sized to `tcp_sndbuf()`. If the send buffer is full, it suspends on
 * `tx_waker` until `on_sent` fires (data ACKed) to make room.
 *
 * ### Error model
 * `on_err` is called by lwIP after a fatal error; the PCB is freed by lwIP before
 * this callback returns, so `pcb` is set to `nullptr` here to prevent any further
 * use.
 */
struct PicoTcpContext {
    tcp_pcb* pcb = nullptr;  // INVALID (null) after on_err fires

    // Receive
    std::vector<uint8_t>           rx_buf;    // accumulated unread bytes
    std::shared_ptr<detail::Waker> rx_waker;  // non-null while read() is suspended
    bool                           rx_eof  = false;

    // Transmit
    std::shared_ptr<detail::Waker> tx_waker;  // non-null while write() is suspended on backpressure

    // Fatal error set by on_err; pcb is null after this
    bool   errored = false;
    err_t  error   = ERR_OK;

    // lwIP static callbacks — registered via tcp_recv/tcp_sent/tcp_err.
    static err_t on_recv(void* arg, tcp_pcb* tpcb, pbuf* p, err_t err);
    static err_t on_sent(void* arg, tcp_pcb* tpcb, uint16_t len);
    static void  on_err (void* arg, err_t err);
};

// ---------------------------------------------------------------------------
// PicoTcpStream
// ---------------------------------------------------------------------------

/**
 * @brief Async TCP connection for Raspberry Pi Pico W.
 *
 * Obtain via `co_await PicoTcpStream::connect(host, port)`.
 * Uses the lwIP raw TCP API; all callbacks fire synchronously from
 * `cyw43_arch_poll()` inside @ref PicoExecutor::wait_for_completion().
 *
 * **Concurrency:** do not `co_await` `read()` and `write()` simultaneously
 * from two different tasks — lwIP's raw API is single-threaded and the
 * shared @ref PicoTcpContext is not protected for concurrent access.
 *
 * **Buffer ownership:** `read()` and `write()` take the buffer by value and
 * return it with the result, matching the @ref ByteBuffer ownership model of
 * the rest of the library.
 *
 * **Destruction:** destroying a `PicoTcpStream` while a `read()` or `write()`
 * coroutine is still live (i.e. not yet awaited to completion) is undefined
 * behaviour. Complete or cancel all I/O before destroying the stream.
 */
class PicoTcpStream {
public:
    PicoTcpStream(PicoTcpStream&&) noexcept;
    PicoTcpStream& operator=(PicoTcpStream&&) noexcept;
    PicoTcpStream(const PicoTcpStream&)            = delete;
    PicoTcpStream& operator=(const PicoTcpStream&) = delete;

    /// Aborts the TCP connection (RST). Does not drain pending I/O.
    ~PicoTcpStream();

    /**
     * @brief Resolves `host` (via lwIP DNS) and opens a TCP connection to `port`.
     *
     * `host` may be a dotted-decimal IPv4 address (resolved synchronously) or a
     * hostname (resolved asynchronously via lwIP's DNS resolver).
     *
     * @throws std::runtime_error on DNS failure or connection refusal.
     */
    [[nodiscard]] static Coro<PicoTcpStream> connect(const char* host, uint16_t port);

    /**
     * @brief Reads up to `buf.size()` bytes and returns `{bytes_read, buf}`.
     *
     * Suspends until at least one byte is available. Returns `{0, buf}` on EOF.
     *
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] Coro<std::pair<std::size_t, Buf>> read(Buf buf);

    /**
     * @brief Writes all bytes in `buf` to the stream and returns `buf`.
     *
     * Handles lwIP send-buffer backpressure: if `tcp_sndbuf()` is full, suspends
     * until `on_sent` frees capacity, then continues.
     *
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     * @throws std::runtime_error on connection error.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] Coro<Buf> write(Buf buf);

private:
    friend class PicoTcpListener;  // constructs PicoTcpStream from accepted PCBs

    explicit PicoTcpStream(std::shared_ptr<PicoTcpContext> ctx);

    std::shared_ptr<PicoTcpContext> m_ctx;
};

} // namespace coro

#include <coro/pico/pico_tcp_stream.hpp>
