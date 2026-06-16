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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

namespace coro {

class TcpListener;
namespace detail { struct LwipTcpCtx; }

/**
 * @brief Async TCP connection. Move-only; obtain via `co_await TcpStream::connect()`.
 *
 * Uses the lwIP raw TCP API; all callbacks fire synchronously from the executor's
 * I/O tick (cyw43_arch_poll() on Pico, sys_check_timeouts() on host test builds).
 *
 * **Concurrency:** do not co_await read() and write() simultaneously from two tasks.
 * **Destruction:** destroying a TcpStream while a read() or write() is in flight is UB.
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
     * @brief Reads up to buf.size() bytes. Returns {bytes_read, buf}; 0 bytes on EOF.
     * @tparam Buf Any type satisfying ByteBuffer.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] Coro<std::pair<std::size_t, Buf>> read(Buf buf) {
        std::size_t n = co_await read_impl(
            reinterpret_cast<std::byte*>(std::ranges::data(buf)),
            std::ranges::size(buf));
        co_return std::pair<std::size_t, Buf>{n, std::move(buf)};
    }

    /**
     * @brief Reads exactly buf.size() bytes. Returns {bytes_read, buf}.
     * bytes_read < buf.size() indicates EOF before the buffer was filled.
     * @tparam Buf Any type satisfying ByteBuffer.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] Coro<std::pair<std::size_t, Buf>> read_exact(Buf buf) {
        auto* data = reinterpret_cast<std::byte*>(std::ranges::data(buf));
        const std::size_t size = std::ranges::size(buf);
        std::size_t total = 0;
        while (total < size) {
            std::size_t n = co_await read_impl(data + total, size - total);
            if (n == 0) break;
            total += n;
        }
        co_return std::pair<std::size_t, Buf>{total, std::move(buf)};
    }

    /**
     * @brief Writes all bytes in buf to the stream. Returns buf after completion.
     * @tparam Buf Any type satisfying ByteBuffer.
     * @throws std::runtime_error on connection error.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] Coro<Buf> write(Buf buf) {
        co_await write_impl(
            reinterpret_cast<const std::byte*>(std::ranges::data(buf)),
            std::ranges::size(buf));
        co_return std::move(buf);
    }

private:
    friend class TcpListener;

    explicit TcpStream(std::shared_ptr<detail::LwipTcpCtx> impl);

    // Defined in tcp_stream_lwip.cpp (or a stub). Never inline — keeps lwIP
    // headers out of this file.
    [[nodiscard]] Coro<std::size_t> read_impl(std::byte* buf, std::size_t size);
    [[nodiscard]] Coro<void>        write_impl(const std::byte* buf, std::size_t size);

    std::shared_ptr<detail::LwipTcpCtx> m_impl;
};

} // namespace coro

#else // !CORO_TCP_BACKEND_LWIP — libuv-backed implementation

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/io/byte_buffer.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/task/join_handle.h>
#include <uv.h>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace coro {

/**
 * @brief Async TCP connection. Satisfies move-only ownership of a connected socket.
 *
 * Obtain a `TcpStream` via `co_await TcpStream::connect(host, port)`.
 * Once connected, use `read()` and `write()` to transfer data. The destructor
 * closes the socket asynchronously on the uv executor.
 *
 * All I/O operations run on the @ref SingleThreadedUvExecutor via `with_context`,
 * wrapping libuv callbacks as @ref UvCallbackResult awaitables.
 *
 * **Concurrency:** a `TcpStream` must not be shared across tasks. Only one
 * `read()` or `write()` future may be in flight at a time.
 *
 * **Buffer ownership:** `read()` and `write()` take ownership of the buffer and
 * return it with the result. This eliminates dangling-pointer bugs at the type
 * system level — no span or raw pointer ever escapes the I/O operation.
 */
class TcpStream {
public:
    using ConnectFuture = JoinHandle<TcpStream>;

    TcpStream(TcpStream&&) noexcept;
    TcpStream& operator=(TcpStream&&) noexcept;
    TcpStream(const TcpStream&)            = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    /// Closes the socket asynchronously on the uv executor. Does not block.
    ~TcpStream();

    /**
     * @brief Resolves `host` and performs an async TCP connect to `port`.
     * @return A `ConnectFuture` that resolves to a connected `TcpStream`.
     * @throws std::system_error on connection failure.
     */
    [[nodiscard]] static ConnectFuture connect(std::string host, uint16_t port);

    /**
     * @brief Reads up to `buf.size()` bytes into `buf` and returns `{bytes_read, buf}`.
     *
     * Takes ownership of `buf`; returns it alongside the byte count so the caller can
     * reuse or inspect the filled portion. Returns 0 bytes on EOF.
     *
     * @tparam Buf Any type satisfying @ref ByteBuffer (e.g. `std::string`, `std::vector<std::byte>`).
     */
    template <ByteBuffer Buf>
    [[nodiscard]] JoinHandle<std::pair<std::size_t, Buf>> read(Buf buf);

    /**
     * @brief Reads exactly `buf.size()` bytes by looping until the buffer is full.
     * Returns `{bytes_read, buf}`; `bytes_read < buf.size()` indicates EOF before
     * the buffer was filled.
     *
     * @tparam Buf Any type satisfying @ref ByteBuffer (e.g. `std::string`, `std::vector<std::byte>`).
     */
    template <ByteBuffer Buf>
    [[nodiscard]] JoinHandle<std::pair<std::size_t, Buf>> read_exact(Buf buf);

    /**
     * @brief Writes all bytes in `buf` to the stream and returns `buf`.
     *
     * Takes ownership of `buf`; returns it after the write completes so the caller can
     * reuse the allocation.
     *
     * @tparam Buf Any type satisfying @ref ByteBuffer (e.g. `std::string`, `std::vector<std::byte>`).
     * @throws std::system_error on write failure.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] JoinHandle<Buf> write(Buf buf);

private:
    friend class TcpListener;

    // Heap-allocated uv_tcp_t — address must be stable across the handle lifetime.
    struct Handle {
        uv_tcp_t handle;
    };

    explicit TcpStream(std::shared_ptr<Handle> handle, SingleThreadedUvExecutor* uv_exec);

    std::shared_ptr<Handle>   m_handle;
    SingleThreadedUvExecutor* m_uv_exec = nullptr;
};

} // namespace coro

#include <coro/io/tcp_stream.hpp>

#endif // CORO_TCP_BACKEND_LWIP
