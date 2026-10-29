#pragma once


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
 * See doc/design/tcp_stream.md. Every operation is a non-blocking call through the
 * sys/tcp.h seam (POSIX sockets, or lwIP on Pico) on the calling thread; a read or
 * write that would block waits for readiness from the Runtime's IoDriver. Requires a
 * Runtime whose executor turns the IoDriver (every Runtime does; a
 * CurrentThreadExecutor given its own Parker does not); `connect()` throws
 * `std::logic_error` otherwise.
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
