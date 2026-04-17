#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/io/ws_stream.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <libwebsockets.h>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace coro::detail::ws {

// ---------------------------------------------------------------------------
// ListenerState — shared between WsListener (worker thread) and the I/O thread.
//
// server_protocol_cb recovers this via lws_context_user(lws_get_context(wsi))
// when LWS_CALLBACK_ESTABLISHED fires for a new incoming connection.
//
// RACE: pending queue and accept_waker are accessed from both the I/O thread
// (server_protocol_cb pushing new connections) and the worker thread (AcceptFuture
// polling). Both accesses are protected by `mutex`.
// ---------------------------------------------------------------------------
struct ListenerState {
    lws_context*                                      ctx = nullptr;  // owned here, destroyed in WsDestroyListenerRequest
    // bind_mutex guards ready, bind_error, and bind_waker.
    // Both WsBindRequest (I/O thread) and BindFuture::poll (worker thread) must hold
    // it when reading or writing any of these fields to avoid race conditions.
    std::mutex                                        bind_mutex;
    bool                                              ready{false};   // set after bind succeeds
    int                                               bind_error = 0; // set before ready=true on failure
    std::shared_ptr<coro::detail::Waker>              bind_waker;     // woken when bind completes
    // accept_mutex guards pending and accept_waker.
    // Both server_protocol_cb (I/O thread) and AcceptFuture::poll (worker thread) must hold
    // it when reading or writing any of these fields.
    std::mutex                                        accept_mutex;
    std::deque<std::shared_ptr<ConnectionState>>      pending;        // accepted, awaiting take by AcceptFuture
    std::shared_ptr<coro::detail::Waker>              accept_waker;   // woken when a connection arrives
    std::atomic<bool>                                 closed{false};

    // lws_context stores a raw pointer into the protocols array — it does NOT copy it.
    // Both must outlive the context (i.e. stay alive until WsDestroyListenerRequest runs).
    std::string         subprotocol_str;        // storage for the protocol name c_str()
    lws_protocols       protocols[2]{};         // [0] = real entry, [1] = null terminator
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
//   void* slot  →  heap-allocated shared_ptr<ConnectionState>*
//
// LWS_CALLBACK_CLOSED deletes the heap-allocated shared_ptr*, decrementing
// the ref count.
// ---------------------------------------------------------------------------
int server_protocol_cb(lws* wsi, lws_callback_reasons reason,
                       void* user, void* in, std::size_t len);

} // namespace coro::detail::ws


namespace coro {

/**
 * @brief Async WebSocket server listener.
 *
 * Binds a TCP port and performs the WebSocket handshake for each incoming
 * connection. Built on libwebsockets using a dedicated server-side lws context
 * that shares the same libuv event loop as the uv executor.
 *
 * Obtain a `WsListener` via `co_await WsListener::bind(host, port)`.
 * Call `co_await listener.accept()` in a loop to receive connections as
 * @ref WsStream objects.
 *
 * Dropping the `WsListener` destroys the server lws context, closing any
 * connections still in the accept queue. Active `WsStream`s that were already
 * handed off are unaffected.
 *
 * All lws handles and callbacks are private implementation details.
 */
class WsListener {
public:
    // -----------------------------------------------------------------------
    // Nested Future types
    // -----------------------------------------------------------------------

    /**
     * @brief Future<WsListener> returned by @ref WsListener::bind().
     *
     * On first poll, submits a `WsBindRequest` to the uv executor, which creates
     * a server-side lws context on the uv thread and registers it on the shared
     * uv_loop. Woken when the context is ready (or fails).
     */
    class BindFuture {
    public:
        using OutputType = WsListener;

        BindFuture(std::string host, uint16_t port,
                   std::vector<std::string> subprotocols, SingleThreadedUvExecutor* uv_exec);

        BindFuture(BindFuture&&) noexcept            = default;
        BindFuture& operator=(BindFuture&&) noexcept = default;
        BindFuture(const BindFuture&)                = delete;
        BindFuture& operator=(const BindFuture&)     = delete;

        PollResult<WsListener> poll(detail::Context& ctx);

    private:
        std::string                                  m_host;
        uint16_t                                     m_port;
        std::vector<std::string>                     m_subprotocols;
        SingleThreadedUvExecutor*                                   m_uv_exec;
        std::shared_ptr<detail::ws::ListenerState>   m_state;  // null until first poll
    };

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
                     SingleThreadedUvExecutor*                                  uv_exec);

        AcceptFuture(AcceptFuture&&) noexcept            = default;
        AcceptFuture& operator=(AcceptFuture&&) noexcept = default;
        AcceptFuture(const AcceptFuture&)                = delete;
        AcceptFuture& operator=(const AcceptFuture&)     = delete;

        PollResult<WsStream> poll(detail::Context& ctx);

    private:
        std::shared_ptr<detail::ws::ListenerState>   m_state;
        SingleThreadedUvExecutor*                                   m_uv_exec;
    };

    // -----------------------------------------------------------------------
    // WsListener public API
    // -----------------------------------------------------------------------

    WsListener(WsListener&&) noexcept;
    WsListener& operator=(WsListener&&) noexcept;
    WsListener(const WsListener&)            = delete;
    WsListener& operator=(const WsListener&) = delete;

    /// Destroys the server lws context on the uv executor. Does not block.
    ~WsListener();

    /**
     * @brief Binds a WebSocket server on `host:port`.
     * @param subprotocols Optional list of subprotocols the server advertises in the
     *        `Sec-WebSocket-Protocol` response header. When empty (the default), the server
     *        accepts connections regardless of any subprotocol the client requests.
     * @return A `BindFuture` that resolves to a bound `WsListener`.
     * @throws std::system_error if the port cannot be bound.
     */
    [[nodiscard]] static BindFuture bind(std::string host, uint16_t port,
                                         std::vector<std::string> subprotocols = {});

    /**
     * @brief Accepts the next incoming WebSocket connection.
     * @return An `AcceptFuture` that resolves to a connected `WsStream`.
     */
    [[nodiscard]] AcceptFuture accept();

private:
    explicit WsListener(std::shared_ptr<detail::ws::ListenerState> state,
                        SingleThreadedUvExecutor*                                  uv_exec);

    std::shared_ptr<detail::ws::ListenerState>   m_state;
    SingleThreadedUvExecutor*                                   m_uv_exec = nullptr;
};

} // namespace coro
