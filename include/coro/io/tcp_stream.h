#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/task/join_handle.h>
#include <uv.h>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

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
 * **Buffer lifetime:** the caller's buffer must remain valid until the future
 * resolves. See CLAUDE.md § non-owning buffer guideline.
 */
class TcpStream {
public:
    using ConnectFuture = JoinHandle<TcpStream>;
    using ReadFuture    = JoinHandle<std::size_t>;
    using WriteFuture   = JoinHandle<void>;

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
     * @brief Reads up to `buf.size()` bytes from the stream.
     * @return A `ReadFuture` resolving to bytes read, or 0 on EOF.
     * The buffer must remain valid until the future resolves.
     */
    [[nodiscard]] ReadFuture read(std::span<std::byte> buf);

    /**
     * @brief Writes all of `data` to the stream.
     * @return A `WriteFuture` resolving when the write is handed to the OS.
     * The data buffer must remain valid until the future resolves.
     * @throws std::system_error on write failure.
     */
    [[nodiscard]] WriteFuture write(std::span<const std::byte> data);

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
