#pragma once

#ifdef CORO_TCP_BACKEND_LWIP

// ---------------------------------------------------------------------------
// lwIP-backed TcpListener (CORO_TCP_BACKEND_LWIP)
// ---------------------------------------------------------------------------

#include <coro/coro.h>
#include <coro/io/tcp_stream.h>
#include <cstdint>
#include <memory>
#include <string>

namespace coro {

namespace detail { struct LwipListenCtx; }

/**
 * @brief Async TCP server. Obtain via `co_await TcpListener::bind(host, port)`.
 *
 * Each call to accept() waits for the next incoming connection and returns a
 * TcpStream. Only one accept() may be in flight at a time.
 *
 * **Destruction:** aborts any queued pending connections and wakes a suspended
 * accept() with a closed error.
 */
class TcpListener {
public:
    TcpListener(TcpListener&&) noexcept;
    TcpListener& operator=(TcpListener&&) noexcept;
    TcpListener(const TcpListener&)            = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    ~TcpListener();

    /**
     * @brief Binds to host:port and starts listening.
     *
     * host must be a dotted-decimal IPv4 string or "0.0.0.0".
     * @throws std::runtime_error on bind or listen failure.
     */
    [[nodiscard]] static Coro<TcpListener> bind(std::string host, uint16_t port);

    /**
     * @brief Waits for the next incoming connection.
     * @throws std::runtime_error if the listener has been closed.
     */
    [[nodiscard]] Coro<TcpStream> accept();

private:
    explicit TcpListener(std::shared_ptr<detail::LwipListenCtx> impl);

    std::shared_ptr<detail::LwipListenCtx> m_impl;
};

} // namespace coro

#else // !CORO_TCP_BACKEND_LWIP — libuv-backed implementation

#include <coro/detail/context.h>
#include <coro/detail/waker.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/join_handle.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <coro/io/tcp_stream.h>
#include <uv.h>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace coro {

/**
 * @brief Async TCP server that accepts incoming connections.
 *
 * Obtain via `co_await TcpListener::bind(host, port)`. Each call to `accept()`
 * returns the next incoming `TcpStream`. The listener must not outlive the
 * `SingleThreadedUvExecutor` it was created on.
 *
 * **Concurrency:** only one `accept()` future may be in flight at a time.
 */
class TcpListener {
public:
    using BindFuture   = JoinHandle<TcpListener>;
    using AcceptFuture = JoinHandle<TcpStream>;

    TcpListener(TcpListener&&) noexcept;
    TcpListener& operator=(TcpListener&&) noexcept;
    TcpListener(const TcpListener&)            = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /// Stops listening and closes the server handle asynchronously on the uv executor.
    ~TcpListener();

    /**
     * @brief Binds and listens on `host:port`.
     * @return A `BindFuture` resolving to a ready `TcpListener`.
     * @throws std::system_error on bind or listen failure.
     */
    [[nodiscard]] static BindFuture bind(std::string host, uint16_t port);

    /**
     * @brief Waits for the next incoming connection.
     * @return An `AcceptFuture` resolving to a connected `TcpStream`.
     * @throws std::runtime_error if the listener has been closed.
     * @throws std::system_error on a fatal accept error.
     */
    [[nodiscard]] AcceptFuture accept();

private:
    // -----------------------------------------------------------------------
    // ListenHandle — shared between TcpListener (owner) and the uv I/O thread.
    //
    // All fields are accessed exclusively on the uv thread; no mutex needed.
    //
    // server.data holds a heap-allocated shared_ptr<ListenHandle> wrapper so
    // that connection_cb always has a stable reference to the state even after
    // TcpListener is destroyed. The wrapper is deleted in the destructor
    // coroutine just before uv_close, after setting closed = true to make any
    // in-flight connection_cb a safe no-op.
    // -----------------------------------------------------------------------
    struct ListenHandle {
        uv_tcp_t                                       server;
        std::deque<std::shared_ptr<TcpStream::Handle>> pending;
        UvCallbackResult<int>*                         accept_notify = nullptr;
        bool                                           closed = false;
    };

    explicit TcpListener(std::shared_ptr<ListenHandle> handle,
                         SingleThreadedUvExecutor* uv_exec);

    // Fired by libuv on the uv thread when a new TCP connection arrives.
    static void connection_cb(uv_stream_t* server, int status);

    std::shared_ptr<ListenHandle> m_handle;
    SingleThreadedUvExecutor*     m_uv_exec = nullptr;
};

} // namespace coro

#endif // CORO_TCP_BACKEND_LWIP
