#pragma once

#include <coro/coro.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/rc.h>
#include <coro/detail/waker.h>
#include <coro/io/ws_stream.h>
#include <libwebsockets.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace coro {

/**
 * @brief Returned by a `process_request` hook to reject an upgrade request.
 *
 * Note: libwebsockets closes the TCP connection when a request is rejected at
 * the filter stage; the status code is informational only and is not sent to
 * the client as an HTTP response.
 */
struct WsUpgradeRejection {
    uint16_t    status_code = 403;
    std::string reason;
};

/**
 * @brief Read-only view of an incoming WebSocket upgrade request.
 *
 * Passed to the `process_request` hook in `WsListener::Options`. Only valid
 * for the duration of the hook call — do not store a copy.
 */
class WsUpgradeRequest {
public:
    explicit WsUpgradeRequest(lws* wsi) : m_wsi(wsi) {}

    /// Returns the request path (e.g. "/chat").
    std::string path() const;

    /// Returns the value of an HTTP request header, or empty string if absent.
    /// Header name is case-insensitive (e.g. "Authorization", "x-api-key").
    std::string header(std::string_view name) const;

    /// Returns the subprotocols offered by the client via Sec-WebSocket-Protocol.
    std::vector<std::string> offered_subprotocols() const;

private:
    lws* m_wsi;
};

} // namespace coro

namespace coro::detail::ws {

// ---------------------------------------------------------------------------
// ListenerState — shared by WsListener and its AcceptFutures (executor threads) and
// server_protocol_cb (the listener's lws service thread), which finds it through
// lws_context_user(). The listener's LwsService keeps it alive until its context is
// destroyed.
// ---------------------------------------------------------------------------
struct ListenerState {
    // accept_mutex guards pending, accept_waker and closed. server_protocol_cb and
    // AcceptFuture::poll both hold it.
    std::mutex                                        accept_mutex;
    std::deque<std::shared_ptr<ConnectionState>>      pending;        // accepted, awaiting accept()
    detail::Weak<detail::Waker>                       accept_waker;   // woken when a connection arrives
    bool                                              closed = false; // the WsListener was dropped

    // lws_context stores a raw pointer into the protocols array — it does NOT copy it.
    // Both must outlive the context, which the LwsService's keep-alive ensures.
    std::string         subprotocol_str;        // storage for the protocol name c_str()
    lws_protocols       protocols[2]{};         // [0] = real entry, [1] = null terminator

    // Options copied from WsListener::Options at bind time. Read-only afterwards.
    coro::WsStream::FrameMode frame_mode       = coro::WsStream::FrameMode::Full;
    std::size_t               max_frame_size   = 4096;
    std::size_t               max_message_size = 0;
    std::function<std::optional<coro::WsUpgradeRejection>(const coro::WsUpgradeRequest&)>
        process_request;
    std::function<std::string(std::span<const std::string_view>)>
        select_subprotocol;
};

// ---------------------------------------------------------------------------
// server_protocol_cb — registered with the server-side lws context.
//
// Unlike protocol_cb (client), incoming wsi objects are not pre-tagged with
// a ConnectionState. We attach one in LWS_CALLBACK_ESTABLISHED using
// per_session_data_size storage (sizeof(void*) bytes managed by lws per wsi).
// The `user` parameter in every callback points to this storage.
//
// Layout of per-session storage:
//   void* slot  →  ConnectionState*, kept alive by its `self` member
//
// CLOSED (or WSI_DESTROY) clears the slot and calls close_connection(), which drops
// `self`.
// ---------------------------------------------------------------------------
int server_protocol_cb(lws* wsi, lws_callback_reasons reason,
                       void* user, void* in, std::size_t len);

} // namespace coro::detail::ws


namespace coro {

/**
 * @brief Async WebSocket server listener.
 *
 * Binds a TCP port and performs the WebSocket handshake for each incoming
 * connection. Built on libwebsockets: each listener has its own lws context and lws
 * service thread (see doc/design/websocket_stream.md, "Service threads"), shared by the
 * streams it accepts.
 *
 * Obtain a `WsListener` via `co_await WsListener::bind(host, port)`.
 * Call `co_await listener.accept()` in a loop to receive connections as
 * @ref WsStream objects.
 *
 * Dropping the `WsListener` rejects new connections and closes those still in the
 * accept queue. `WsStream`s already handed off are unaffected: they keep the context,
 * and with it the listening socket, until the last of them is dropped. Until then
 * the port stays bound, and new connections to it fail the upgrade.
 *
 * All lws handles and callbacks are private implementation details.
 */
class WsListener {
public:
    /**
     * @brief Server-wide options for WsListener::bind().
     *
     * All fields have sensible defaults. Use designated initializers:
     * @code
     *   co_await WsListener::bind("localhost", 8080, {
     *       .max_frame_size = 65536,
     *       .process_request = [](const WsUpgradeRequest& req) {
     *           if (!req.header("Authorization").starts_with("Bearer "))
     *               return std::optional{WsUpgradeRejection{401, "Unauthorized"}};
     *           return std::optional<WsUpgradeRejection>{};
     *       },
     *   });
     * @endcode
     */
    struct Options {
        WsStream::FrameMode  frame_mode       = WsStream::FrameMode::Full;
        std::size_t          max_frame_size   = 4096;   ///< lws rx_buffer_size per connection.
        std::size_t          max_message_size = 0;      ///< 0 = unlimited; enforced in receive callback.
        std::vector<std::string> subprotocols = {};     ///< Subprotocols advertised in the server handshake.

        /// Called before the WebSocket upgrade is accepted. Return a
        /// WsUpgradeRejection to reject, or std::nullopt to accept.
        std::function<std::optional<WsUpgradeRejection>(const WsUpgradeRequest&)>
            process_request;

        /// Called with the subprotocols offered by the client. Return the
        /// chosen subprotocol name, or empty string to reject the connection.
        std::function<std::string(std::span<const std::string_view>)>
            select_subprotocol;
    };

    // -----------------------------------------------------------------------
    // Nested Future types
    // -----------------------------------------------------------------------

    /**
     * @brief Future<WsStream> returned by @ref WsListener::accept().
     *
     * Suspends until a client completes the WebSocket handshake. The resulting
     * `WsStream` takes ownership of the connection's `ConnectionState`.
     */
    class AcceptFuture {
    public:
        using OutputType = WsStream;

        AcceptFuture(std::shared_ptr<detail::ws::ListenerState> state,
                     std::shared_ptr<detail::ws::LwsService>    service);

        AcceptFuture(AcceptFuture&&) noexcept            = default;
        AcceptFuture& operator=(AcceptFuture&&) noexcept = default;
        AcceptFuture(const AcceptFuture&)                = delete;
        AcceptFuture& operator=(const AcceptFuture&)     = delete;

        PollResult<WsStream> poll(detail::Context& ctx);

    private:
        std::shared_ptr<detail::ws::ListenerState>   m_state;
        std::shared_ptr<detail::ws::LwsService>      m_service;
    };

    // -----------------------------------------------------------------------
    // WsListener public API
    // -----------------------------------------------------------------------

    WsListener(WsListener&&) noexcept;
    WsListener& operator=(WsListener&&) noexcept;
    WsListener(const WsListener&)            = delete;
    WsListener& operator=(const WsListener&) = delete;

    /// Stops accepting (see the class comment). Does not block, unless no accepted
    /// stream is left: then it destroys the context and joins its service thread.
    ~WsListener();

    /**
     * @brief Binds a WebSocket server on `host:port`.
     *
     * An empty `host` listens on every interface. Otherwise it's resolved with
     * `lookup_host()` and the first address that binds is used. The context, and its
     * listening socket, are created before this returns.
     *
     * @param options Frame mode and sizes, advertised subprotocols (none by default,
     *        which accepts whatever the client requests) and upgrade hooks.
     * @throws std::system_error with `dns_error_category()` if `host` doesn't resolve,
     *         or the last address's bind error (e.g. `EADDRINUSE`) if none binds.
     */
    [[nodiscard]] static Coro<WsListener> bind(std::string host, uint16_t port);
    [[nodiscard]] static Coro<WsListener> bind(std::string host, uint16_t port, Options options);

    /**
     * @brief Accepts the next incoming WebSocket connection.
     * @return An `AcceptFuture` that resolves to a connected `WsStream`.
     */
    [[nodiscard]] AcceptFuture accept();

private:
    WsListener(std::shared_ptr<detail::ws::ListenerState> state,
               std::shared_ptr<detail::ws::LwsService>    service);

    /// Marks the listener closed, fails pending accepts, and closes the connections
    /// queued for accept(). Used by the destructor and move assignment.
    void close() noexcept;

    std::shared_ptr<detail::ws::ListenerState>   m_state;
    std::shared_ptr<detail::ws::LwsService>      m_service;
};

} // namespace coro
