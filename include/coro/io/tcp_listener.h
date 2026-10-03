#pragma once

#ifdef CORO_TCP_BACKEND_LWIP

// ---------------------------------------------------------------------------
// lwIP-backed TcpListener (CORO_TCP_BACKEND_LWIP)
// ---------------------------------------------------------------------------

#include <coro/coro.h>
#include <coro/io/tcp_stream.h>
#include <coro/detail/rc.h>
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
    explicit TcpListener(detail::Rc<detail::LwipListenCtx> impl);

    detail::Rc<detail::LwipListenCtx> m_impl;
};

} // namespace coro

#else // !CORO_TCP_BACKEND_LWIP — desktop implementation on the IoDriver

#include <coro/coro.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/socket_state.h>
#include <coro/io/tcp_stream.h>
#include <cstdint>
#include <memory>
#include <string>

namespace coro {

/**
 * @brief Future returned by `TcpListener::accept()`. Yields the next connection.
 *
 * A leaf future: dropping it mid-wait is safe and accepts nothing, so `accept()` can
 * be raced against a timeout or a shutdown signal with `select`.
 */
class TcpAcceptFuture {
public:
    using OutputType = TcpStream;

    explicit TcpAcceptFuture(std::shared_ptr<detail::SocketState> listener)
        : m_listener(std::move(listener)) {}

    PollResult<TcpStream> poll(detail::Context& ctx);

private:
    std::shared_ptr<detail::SocketState> m_listener;
};

/**
 * @brief Async TCP server that accepts incoming connections.
 *
 * Obtain via `co_await TcpListener::bind(host, port)`; each `co_await accept()`
 * returns the next incoming `TcpStream`. See doc/design/tcp_stream.md, "TcpListener".
 * Requires an executor that turns the IoDriver (`Runtime(n)` for any n); `bind()`
 * throws `std::logic_error` otherwise.
 *
 * **Concurrency:** only one `accept()` may be in flight at a time.
 *
 * **Closing:** the listening socket is closed synchronously when the last owner drops
 * it: the TcpListener or a still-pending accept() (which keeps listening until it
 * completes or is dropped). Connections still in the kernel's backlog are reset.
 */
class TcpListener {
public:
    TcpListener(TcpListener&&) noexcept;
    TcpListener& operator=(TcpListener&&) noexcept;
    TcpListener(const TcpListener&)            = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /// Releases this handle; see Closing above.
    ~TcpListener();

    /**
     * @brief Binds and listens on `host:port`. `host` is an IPv4 or IPv6 literal
     * ("0.0.0.0", "127.0.0.1", "::", "::1") or a name resolved with @ref lookup_host;
     * the first resolved address that binds is used. SO_REUSEADDR is set, so a
     * restarted server can rebind while old connections sit in TIME_WAIT.
     * @throws std::system_error (at co_await) if host doesn't resolve, or with the
     *         last address's bind or listen failure (e.g. EADDRINUSE).
     * @throws std::logic_error if the current Runtime's executor doesn't turn the
     *         IoDriver.
     */
    [[nodiscard]] static Coro<TcpListener> bind(std::string host, uint16_t port);

    /**
     * @brief Waits for the next incoming connection.
     * @throws std::system_error (at co_await) on a listener error, e.g. EMFILE.
     */
    [[nodiscard]] TcpAcceptFuture accept();

private:
    using State = detail::SocketState;

    explicit TcpListener(std::shared_ptr<State> state);

    std::shared_ptr<State> m_state;
};

} // namespace coro

#endif // CORO_TCP_BACKEND_LWIP
