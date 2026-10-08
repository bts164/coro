#pragma once

#ifdef CORO_TCP_BACKEND_LWIP

// ---------------------------------------------------------------------------
// lwIP-backed TcpStream (CORO_TCP_BACKEND_LWIP)
//
// Backed by the lwIP raw TCP API in NO_SYS mode. All callbacks fire
// synchronously on the executor thread during cyw43_arch_poll() /
// sys_check_timeouts(). No lwIP headers appear here — the implementation
// is compiled separately via src/io/lwip/tcp_stream_lwip.cpp.
//
// To integrate in a project:
//   target_sources(my_app PRIVATE ${CORO_ROOT}/src/io/lwip/tcp_stream_lwip.cpp)
//   target_link_libraries(my_app PRIVATE coro::coro lwip)
// ---------------------------------------------------------------------------

#include <coro/coro.h>
#include <coro/io/byte_buffer.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/rc.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

namespace coro {

class TcpListener;

namespace detail {

struct LwipTcpCtx;

// The non-template halves of the read and write futures below. Defined in
// tcp_stream_lwip.cpp, which keeps the lwIP headers out of this file.

/// Copies buffered bytes into `buf[filled, size)` and advances `filled`. Ready after
/// the first chunk (`exact = false`), or once the buffer is full or the peer has
/// closed (`exact = true`). Pending, with the context's waker stored as the
/// connection's receive waker, when it needs bytes that haven't arrived. Bytes
/// received before a connection error are delivered before the error is reported.
PollResult<void> lwip_tcp_poll_read(LwipTcpCtx& tcp, std::byte* buf, std::size_t size,
                                    std::size_t& filled, bool exact, Context& ctx);

/// Queues `buf[written, size)` with lwIP as send-buffer space allows and advances
/// `written`. Ready once everything is queued and flushed. Pending, with the
/// context's waker stored as the connection's send waker, when the send buffer is
/// full.
PollResult<void> lwip_tcp_poll_write(LwipTcpCtx& tcp, const std::byte* buf, std::size_t size,
                                     std::size_t& written, Context& ctx);

/// Drops the connection's receive (or send) waker, for a future that is destroyed
/// while pending.
void lwip_tcp_cancel_read(LwipTcpCtx& tcp) noexcept;
void lwip_tcp_cancel_write(LwipTcpCtx& tcp) noexcept;

} // namespace detail

/**
 * @brief Future returned by `TcpStream::read()` (`Exact = false`) and `read_exact()`
 * (`Exact = true`). Yields `{bytes_read, buf}`.
 *
 * Hand-written rather than a `Coro`, so a read has no coroutine frame and allocates
 * nothing. When bytes are already buffered, the first `poll()` copies them out and
 * returns ready with no suspension.
 *
 * `read()` returns after the first non-empty chunk, or 0 at EOF. `read_exact()` keeps
 * going until the buffer is full or EOF; its progress lives in the future, so a
 * dropped `read_exact()` loses the bytes it had already copied.
 *
 * Dropping it while pending is safe: the destructor takes its waker back out of the
 * connection. It shares ownership of the connection state, so it may outlive the
 * TcpStream, but then it never completes.
 *
 * Race note: lwIP callbacks run on the executor thread, never concurrently with
 * `poll()`. Overlapping reads on one stream aren't supported: the second would
 * replace the first's waker, and the two would split the byte stream between them.
 */
template<ByteBuffer Buf, bool Exact>
class TcpReadFuture {
public:
    using OutputType = std::pair<std::size_t, Buf>;

    TcpReadFuture(detail::Rc<detail::LwipTcpCtx> tcp, Buf buf)
        : m_tcp(std::move(tcp)), m_buf(std::move(buf)) {}

    // A moved-from future has no connection, so its destructor does nothing.
    TcpReadFuture(TcpReadFuture&&) noexcept            = default;
    TcpReadFuture& operator=(TcpReadFuture&&) noexcept = delete;

    ~TcpReadFuture() {
        if (m_tcp && m_waiting) detail::lwip_tcp_cancel_read(*m_tcp);
    }

    PollResult<OutputType> poll(detail::Context& ctx) {
        auto result = detail::lwip_tcp_poll_read(*m_tcp,
            reinterpret_cast<std::byte*>(std::ranges::data(m_buf)), std::ranges::size(m_buf),
            m_filled, Exact, ctx);
        m_waiting = result.isPending();
        if (m_waiting)        return PollPending;
        if (result.isError()) return PollError(result.error());
        return OutputType{m_filled, std::move(m_buf)};
    }

private:
    detail::Rc<detail::LwipTcpCtx> m_tcp;
    Buf                            m_buf;
    std::size_t                    m_filled  = 0;
    // True while this future's waker may be the connection's receive waker.
    bool                           m_waiting = false;
};

/**
 * @brief Future returned by `TcpStream::write()`. Yields `buf` once every byte has
 * been handed to lwIP.
 *
 * Hand-written for the same reason as `TcpReadFuture`: when lwIP's send buffer has
 * room, the first `poll()` queues everything with no coroutine frame. Otherwise it
 * keeps its progress and waits for acknowledgements to free space.
 *
 * Dropping it while pending is memory-safe, but may leave part of the buffer sent.
 *
 * Race note: as for `TcpReadFuture`. Overlapping writes on one stream aren't
 * supported: the second would replace the first's waker, and their bytes could
 * interleave.
 */
template<ByteBuffer Buf>
class TcpWriteFuture {
public:
    using OutputType = Buf;

    TcpWriteFuture(detail::Rc<detail::LwipTcpCtx> tcp, Buf buf)
        : m_tcp(std::move(tcp)), m_buf(std::move(buf)) {}

    TcpWriteFuture(TcpWriteFuture&&) noexcept            = default;
    TcpWriteFuture& operator=(TcpWriteFuture&&) noexcept = delete;

    ~TcpWriteFuture() {
        if (m_tcp && m_waiting) detail::lwip_tcp_cancel_write(*m_tcp);
    }

    PollResult<Buf> poll(detail::Context& ctx) {
        auto result = detail::lwip_tcp_poll_write(*m_tcp,
            reinterpret_cast<const std::byte*>(std::ranges::data(m_buf)),
            std::ranges::size(m_buf), m_written, ctx);
        m_waiting = result.isPending();
        if (m_waiting)        return PollPending;
        if (result.isError()) return PollError(result.error());
        return std::move(m_buf);
    }

private:
    detail::Rc<detail::LwipTcpCtx> m_tcp;
    Buf                            m_buf;
    std::size_t                    m_written = 0;
    // True while this future's waker may be the connection's send waker.
    bool                           m_waiting = false;
};

/**
 * @brief Async TCP connection. Move-only; obtain via `co_await TcpStream::connect()`.
 *
 * Uses the lwIP raw TCP API; all callbacks fire synchronously from the executor's
 * I/O tick (cyw43_arch_poll() on Pico, sys_check_timeouts() on host test builds).
 *
 * **Concurrency:** only one read (`read()`/`read_exact()`) and only one `write()` may
 * be in flight at a time. A read and a write may be in flight together.
 *
 * **Cancellation:** dropping a pending `read()` is safe and loses no data. Dropping a
 * pending `read_exact()` loses the bytes it already read, and dropping a pending
 * `write()` may leave part of the buffer sent: after either, the byte stream is out
 * of step and the connection should be closed.
 *
 * **Destruction:** a read or write still pending when the TcpStream is destroyed
 * never completes; drop it.
 */
class TcpStream {
public:
    TcpStream(TcpStream&&) noexcept;
    TcpStream& operator=(TcpStream&&) noexcept;
    TcpStream(const TcpStream&)            = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    ~TcpStream();

    /**
     * @brief Resolves host (dotted-decimal or hostname via lwIP DNS) and connects to port.
     * @throws std::runtime_error on DNS failure or connection refusal.
     */
    [[nodiscard]] static Coro<TcpStream> connect(std::string host, uint16_t port);

    /**
     * @brief Reads up to buf.size() bytes. Returns {bytes_read, buf}; 0 bytes on EOF
     * or for an empty buf.
     * @tparam Buf Any type satisfying ByteBuffer.
     * @throws std::runtime_error (at co_await) on connection error.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] TcpReadFuture<Buf, false> read(Buf buf) {
        return TcpReadFuture<Buf, false>(m_impl, std::move(buf));
    }

    /**
     * @brief Reads exactly buf.size() bytes. Returns {bytes_read, buf}.
     * bytes_read < buf.size() indicates EOF before the buffer was filled.
     * @tparam Buf Any type satisfying ByteBuffer.
     * @throws std::runtime_error (at co_await) on connection error.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] TcpReadFuture<Buf, true> read_exact(Buf buf) {
        return TcpReadFuture<Buf, true>(m_impl, std::move(buf));
    }

    /**
     * @brief Writes all bytes in buf to the stream. Returns buf after completion.
     * @tparam Buf Any type satisfying ByteBuffer.
     * @throws std::runtime_error (at co_await) on connection error.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] TcpWriteFuture<Buf> write(Buf buf) {
        return TcpWriteFuture<Buf>(m_impl, std::move(buf));
    }

private:
    friend class TcpListener;

    explicit TcpStream(detail::Rc<detail::LwipTcpCtx> impl);

    detail::Rc<detail::LwipTcpCtx> m_impl;
};

} // namespace coro

#else // !CORO_TCP_BACKEND_LWIP — desktop implementation on the IoDriver

#include <coro/coro.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/socket_state.h>
#include <coro/detail/stream_io.h>
#include <coro/detail/sys/tcp.h>
#include <coro/io/byte_buffer.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace coro {

class TcpListener;
class TcpAcceptFuture;

namespace detail {
/// TcpStream's syscalls for the shared byte-stream futures (detail/stream_io.h).
struct TcpIo {
    static sys::IoResult try_read(sys::RawFd fd, std::byte* data, std::size_t size) noexcept {
        return sys::tcp_try_read(fd, data, size);
    }
    static sys::IoResult try_write(sys::RawFd fd, const std::byte* data,
                                   std::size_t size) noexcept {
        return sys::tcp_try_write(fd, data, size);
    }
    static constexpr const char* read_name       = "TcpStream::read";
    static constexpr const char* read_exact_name = "TcpStream::read_exact";
    static constexpr const char* write_name      = "TcpStream::write";
};
} // namespace detail

/// Future returned by `TcpStream::read()` (`Exact = false`) and `read_exact()`
/// (`Exact = true`). Yields `{bytes_read, buf}`.
template<ByteBuffer Buf, bool Exact>
using TcpReadFuture = detail::FdReadFuture<detail::TcpIo, Buf, Exact>;

/// Future returned by `TcpStream::write()`. Yields `buf` once every byte is written.
template<ByteBuffer Buf>
using TcpWriteFuture = detail::FdWriteFuture<detail::TcpIo, Buf>;

/**
 * @brief Async TCP connection. Move-only; obtain via `co_await TcpStream::connect()` or
 * `co_await listener.accept()`.
 *
 * See doc/design/tcp_stream.md. Every operation is a non-blocking syscall
 * on the calling thread; a read or write that would block waits for readiness from
 * the Runtime's IoDriver. Requires an executor that turns the IoDriver (`Runtime(n)`
 * for any n); `connect()` throws `std::logic_error` otherwise.
 *
 * **Concurrency:** only one read (`read()`/`read_exact()`) and only one `write()` may
 * be in flight at a time; a read and a write may run concurrently, on any threads.
 *
 * **Cancellation:** dropping a pending `read()` is always safe and loses no data.
 * Dropping a pending `read_exact()` loses the bytes it already read, and dropping a
 * pending `write()` may leave part of the buffer sent: after either, the byte stream
 * is out of step and the connection should be closed.
 *
 * **Buffer ownership:** reads and writes take ownership of the buffer and return it
 * with the result, so no span or raw pointer ever escapes the I/O operation.
 *
 * **Closing:** the socket is closed synchronously when the last owner drops it: the
 * TcpStream or a still-pending future (which keeps the connection open until it
 * completes or is dropped).
 */
class TcpStream {
public:
    TcpStream(TcpStream&&) noexcept;
    TcpStream& operator=(TcpStream&&) noexcept;
    TcpStream(const TcpStream&)            = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    /// Releases this handle. The socket is deregistered and closed synchronously once
    /// no in-flight future still uses it.
    ~TcpStream();

    /**
     * @brief Connects to `host:port`. `host` is a name ("localhost", resolved with
     * @ref lookup_host on the blocking pool) or an IPv4 or IPv6 literal ("127.0.0.1",
     * "::1"). Each resolved address is tried in turn until one connects.
     * @throws std::system_error (at co_await) if host doesn't resolve (code in
     *         @ref dns_error_category()), or with the last address's error if none
     *         connects (e.g. ECONNREFUSED).
     * @throws std::logic_error if the current Runtime's executor doesn't turn the
     *         IoDriver.
     */
    [[nodiscard]] static Coro<TcpStream> connect(std::string host, uint16_t port);

    /**
     * @brief Reads up to `buf.size()` bytes into `buf` and returns `{bytes_read, buf}`;
     * 0 bytes on EOF.
     *
     * Returns a hand-written future (not a Coro): when data is already buffered, the
     * first poll() completes it with no allocation.
     *
     * @tparam Buf Any type satisfying @ref ByteBuffer (e.g. `std::string`, `std::vector<std::byte>`).
     * @throws std::system_error (at co_await) on a connection error, e.g. ECONNRESET.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] TcpReadFuture<Buf, false> read(Buf buf);

    /**
     * @brief Reads exactly `buf.size()` bytes. Returns `{bytes_read, buf}`;
     * `bytes_read < buf.size()` indicates EOF before the buffer was filled.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     * @throws std::system_error (at co_await) on a connection error.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] TcpReadFuture<Buf, true> read_exact(Buf buf);

    /**
     * @brief Writes all of `buf` to the stream and returns `buf` once the kernel has
     * accepted every byte.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     * @throws std::system_error (at co_await) on a connection error, e.g. EPIPE.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] TcpWriteFuture<Buf> write(Buf buf);

private:
    friend class TcpListener;
    friend class TcpAcceptFuture;

    using State = detail::SocketState;

    explicit TcpStream(std::shared_ptr<State> state);

    std::shared_ptr<State> m_state;
};

} // namespace coro

#include <coro/io/tcp_stream.hpp>

#endif // CORO_TCP_BACKEND_LWIP
