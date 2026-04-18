#pragma once

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
