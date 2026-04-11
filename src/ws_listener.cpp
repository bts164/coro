#include <coro/io/ws_listener.h>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace coro {

// =============================================================================
// IoRequest subtypes — anonymous namespace; IoService has no knowledge of these.
// =============================================================================

namespace {

using namespace coro::detail::ws;

// ---------------------------------------------------------------------------
// WsBindRequest
//
// Submitted by BindFuture on first poll. Runs on the I/O thread: creates a
// server-side lws context that listens on the given port and registers its
// handles on the shared uv_loop. Stores a heap-allocated shared_ptr<ListenerState>
// as the lws context user data so server_protocol_cb can find the ListenerState
// from any callback via lws_context_user(lws_get_context(wsi)).
// ---------------------------------------------------------------------------
struct WsBindRequest : coro::IoRequest {
    std::shared_ptr<ListenerState> state;
    std::string                    host;
    uint16_t                       port;
    std::string                    subprotocol_str;  // empty = accept any subprotocol

    WsBindRequest(std::shared_ptr<ListenerState> s, std::string h, uint16_t p, std::string proto)
        : state(std::move(s)), host(std::move(h)), port(p), subprotocol_str(std::move(proto)) {}

    void execute(uv_loop_t* loop) override {
        // Populate the protocols array on ListenerState rather than on the stack.
        // lws_context stores a raw pointer into this array — it does NOT copy it —
        // so the array must outlive the context (i.e. until WsDestroyListenerRequest).
        //
        // The name field controls Sec-WebSocket-Protocol negotiation.
        // "coro-ws" is used as the default name (nullptr would be misread as the
        // terminator entry by lws and silently disable the protocol).
        // A client sending no Sec-WebSocket-Protocol header still matches the first
        // registered protocol, so the default name is never exposed to the peer.
        state->subprotocol_str = std::move(subprotocol_str);
        // Use "coro-ws" as the default name when no subprotocol is specified.
        // nullptr would be mistaken for the terminator entry by lws.
        const char* proto_name = state->subprotocol_str.empty()
                                     ? "coro-ws" : state->subprotocol_str.c_str();
        state->protocols[0] = { proto_name, server_protocol_cb,
                                 sizeof(void*), 4096, 0, nullptr, 0 };
        state->protocols[1] = { nullptr, nullptr, 0, 0, 0, nullptr, 0 };
        const lws_protocols* protocols = state->protocols;

        // Heap-allocate a shared_ptr wrapper as context user data.
        // server_protocol_cb recovers it via lws_context_user(); it is deleted
        // by WsDestroyListenerRequest after lws_context_destroy().
        auto* sp = new std::shared_ptr<ListenerState>(state);

        lws_context_creation_info info{};
        info.port            = port;
        info.iface           = host.empty() ? nullptr : host.c_str();
        info.protocols       = protocols;
        info.options        |= LWS_SERVER_OPTION_LIBUV;
        void *loop_ptr = loop;  // loop is already uv_loop_t*; don't take its address again
        info.foreign_loops = &loop_ptr;
        info.user            = sp;  // recovered via lws_context_user()

        state->ctx = lws_create_context(&info);
        if (!state->ctx) {
            delete sp;
        }

        // Wake the BindFuture regardless of success or failure.
        std::shared_ptr<detail::Waker> waker_to_wake;
        {
            std::lock_guard lk(state->bind_mutex);
            if (!state->ctx) {
                state->bind_error = -1;
            }
            state->ready = true;
            waker_to_wake = state->bind_waker;
        }
        if (waker_to_wake)
            waker_to_wake->wake();
    }
};

// ---------------------------------------------------------------------------
// WsDestroyListenerRequest
//
// Submitted by WsListener::~WsListener(). Destroys the server lws context on
// the I/O thread (required — lws is not thread-safe), then deletes the
// heap-allocated shared_ptr<ListenerState> stored as context user data.
// ---------------------------------------------------------------------------
struct WsDestroyListenerRequest : coro::IoRequest {
    std::shared_ptr<ListenerState> state;
    explicit WsDestroyListenerRequest(std::shared_ptr<ListenerState> s)
        : state(std::move(s)) {}

    void execute(uv_loop_t* /*loop*/) override {
        if (!state->ctx) return;
        // Recover and delete the shared_ptr<ListenerState> wrapper stored as
        // context user data before calling lws_context_destroy().
        auto* sp = static_cast<std::shared_ptr<ListenerState>*>(
                       lws_context_user(state->ctx));
        lws_context_destroy(state->ctx);
        state->ctx = nullptr;
        state->closed.store(true, std::memory_order_release);
        delete sp;

        // Wake any AcceptFuture blocked on this listener so it can return an error.
        std::lock_guard lk(state->accept_mutex);
        if (state->accept_waker)
            state->accept_waker->wake();
    }
};

} // anonymous namespace

// =============================================================================
// coro::detail::ws — server_protocol_cb
// =============================================================================

namespace detail::ws {

#define LOGSTDOUT(...)
    // do { \
    //     std::printf("server_protocol_cb:%d -  ", __LINE__); \
    //     std::printf(__VA_ARGS__); \
    //     std::fflush(stdout); \
    // } while (0)

int server_protocol_cb(lws* wsi, lws_callback_reasons reason,
                        void* user, void* in, std::size_t len) {

    // `user` points to sizeof(void*) bytes of lws-managed per-session storage.
    // We use this slot to hold a heap-allocated shared_ptr<ConnectionState>*.
    // On ESTABLISHED we write it; on CLOSED we delete it.
    auto** slot = static_cast<void**>(user);

    switch (reason) {

    // -----------------------------------------------------------------------
    // New client completed the WebSocket handshake.
    // -----------------------------------------------------------------------
    case LWS_CALLBACK_ESTABLISHED: {
        LOGSTDOUT("connection established\n");
        // Recover the ListenerState from lws context user data.
        auto* listener_sp = static_cast<std::shared_ptr<ListenerState>*>(
                                lws_context_user(lws_get_context(wsi)));
        if (!listener_sp) {
            LOGSTDOUT("no listener state, rejecting\n");
            return -1;
        }
        auto& listener = **listener_sp;

        // Create a ConnectionState for this connection and store it in the
        // per-session slot so subsequent callbacks can find it.
        auto conn = std::make_shared<ConnectionState>();
        conn->wsi        = wsi;
        conn->frame_mode = WsStream::FrameMode::Full;  // server always uses Full for now
        auto* conn_sp = new std::shared_ptr<ConnectionState>(conn);
        *slot = conn_sp;  // write into lws-managed per-session storage

        // Enqueue the connection for the next accept() call.
        {
            std::lock_guard lk(listener.accept_mutex);
            listener.pending.push_back(conn);
            if (listener.accept_waker)
                listener.accept_waker->wake();
        }
        LOGSTDOUT("connection enqueued for accept()\n");
        break;
    }

    // -----------------------------------------------------------------------
    // Incoming data — delegate to shared receive logic.
    // -----------------------------------------------------------------------
    case LWS_CALLBACK_RECEIVE: {
        if (!slot || !*slot) break;
        auto& state = **static_cast<std::shared_ptr<ConnectionState>*>(*slot);

        bool final_fragment = lws_is_final_fragment(wsi);
        bool is_text        = (lws_frame_is_binary(wsi) == 0);
        auto* bytes         = static_cast<const std::byte*>(in);

        LOGSTDOUT("received %zu bytes (final=%d, text=%d)\n", len, final_fragment, is_text);
        std::shared_ptr<detail::Waker> waker_to_wake;
        {
            std::lock_guard lk(state.receive.mutex);
            if (state.receive.cancelled) {
                LOGSTDOUT("receive cancelled, discarding data\n");
                state.receive.buffer.clear();
                break;
            }
            state.receive.buffer.insert(state.receive.buffer.end(), bytes, bytes + len);

            if (state.frame_mode == WsStream::FrameMode::Partial || final_fragment) {
                LOGSTDOUT("message complete, waking receiver\n");
                state.receive.is_text  = is_text;
                state.receive.is_final = final_fragment;
                state.receive.complete = true;
                waker_to_wake = state.receive.waker.load();
            } else {
                LOGSTDOUT("message fragment received, waiting for more\n");
            }
        }
        if (waker_to_wake) waker_to_wake->wake();
        break;
    }

    // -----------------------------------------------------------------------
    // Write-ready — pop and send the front of the send queue.
    // -----------------------------------------------------------------------
    case LWS_CALLBACK_SERVER_WRITEABLE: {
        LOGSTDOUT("writeable\n");
        if (!slot || !*slot) break;
        auto& state = **static_cast<std::shared_ptr<ConnectionState>*>(*slot);

        // Close takes priority over pending sends.
        if (state.closing.load(std::memory_order_acquire)) {
            LOGSTDOUT("closing connection\n");
            lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
            return -1;
        }

        std::shared_ptr<SendSubState> sub;
        {
            std::lock_guard lk(state.send_queue_mutex);
            if (state.send_queue.empty()) {
                LOGSTDOUT("writeable but send queue is empty\n");
                break;
            }
            sub = state.send_queue.front();
            state.send_queue.pop_front();
        }

        std::shared_ptr<detail::Waker> waker_to_wake;
        {
            std::lock_guard lk(sub->mutex);
            if (!sub->cancelled) {
                // Copy data under the lock before touching the span — the destructor
                // sets cancelled and the caller may free their buffer concurrently.
                std::vector<std::byte> buf(LWS_PRE + sub->data.size());
                std::copy(sub->data.begin(), sub->data.end(), buf.begin() + LWS_PRE);
                int flags = (sub->opcode == WsStream::OpCode::Binary)
                                ? LWS_WRITE_BINARY : LWS_WRITE_TEXT;
                int r = lws_write(wsi,
                                  reinterpret_cast<unsigned char*>(buf.data() + LWS_PRE),
                                  buf.size() - LWS_PRE,
                                  static_cast<lws_write_protocol>(flags));
                if (r < 0) {
                    LOGSTDOUT("lws_write error %d\n", r);
                } else {
                    LOGSTDOUT("sent %zu bytes\n", buf.size() - LWS_PRE);
                }
                sub->error = (r < 0) ? r : 0;
                sub->data  = {};
            } else {
                LOGSTDOUT("send cancelled, skipping\n");
            }
            sub->complete = true;
            waker_to_wake = sub->waker.load();
        }
        if (waker_to_wake) {
            LOGSTDOUT("send complete, waking sender\n");
            waker_to_wake->wake();
        } else {
            LOGSTDOUT("no waker to wake\n");
        }
        {
            std::lock_guard lk(state.send_queue_mutex);
            if (!state.send_queue.empty()) {
                LOGSTDOUT("more sends queued, requesting another writeable callback\n");
                lws_callback_on_writable(wsi);
            } else {
                LOGSTDOUT("no more sends queued\n");
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    // Connection closed — wake pending futures and free the session state.
    // -----------------------------------------------------------------------
    case LWS_CALLBACK_CLOSED: {
        LOGSTDOUT("connection closed\n");
        if (!slot || !*slot) break;
        auto* conn_sp = static_cast<std::shared_ptr<ConnectionState>*>(*slot);
        auto& state   = **conn_sp;

        state.wsi = nullptr;
        state.closed.store(true, std::memory_order_release);

        std::shared_ptr<detail::Waker> recv_waker;
        {
            std::lock_guard lk(state.receive.mutex);
            recv_waker = state.receive.waker.load();
        }
        if (recv_waker) recv_waker->wake();
        {
            std::lock_guard lk(state.send_queue_mutex);
            for (auto& sub : state.send_queue) {
                std::shared_ptr<detail::Waker> send_waker;
                {
                    std::lock_guard slk(sub->mutex);
                    sub->error    = -1;
                    sub->complete = true;
                    send_waker    = sub->waker.load();
                }
                if (send_waker) send_waker->wake();
            }
            state.send_queue.clear();
        }

        // Delete the heap-allocated shared_ptr wrapper; may free ConnectionState.
        delete conn_sp;
        *slot = nullptr;
        break;
    }

    default:
        break;
    }
    return 0;
}

} // namespace detail::ws

// =============================================================================
// WsListener
// =============================================================================

WsListener::WsListener(std::shared_ptr<detail::ws::ListenerState> state,
                        IoService* io_service)
    : m_state(std::move(state))
    , m_io_service(io_service) {}

WsListener::WsListener(WsListener&&) noexcept = default;
WsListener& WsListener::operator=(WsListener&&) noexcept = default;

WsListener::~WsListener() {
    if (m_state)
        m_io_service->submit(
            std::make_unique<WsDestroyListenerRequest>(std::move(m_state)));
}

WsListener::BindFuture WsListener::bind(std::string host, uint16_t port,
                                          std::vector<std::string> subprotocols) {
    return BindFuture(std::move(host), port, std::move(subprotocols), &current_io_service());
}

WsListener::AcceptFuture WsListener::accept() {
    return AcceptFuture(m_state, m_io_service);
}

// =============================================================================
// BindFuture
// =============================================================================

WsListener::BindFuture::BindFuture(std::string host, uint16_t port,
                                     std::vector<std::string> subprotocols,
                                     IoService* io_service)
    : m_host(std::move(host))
    , m_port(port)
    , m_subprotocols(std::move(subprotocols))
    , m_io_service(io_service) {}

PollResult<WsListener> WsListener::BindFuture::poll(detail::Context& ctx) {
    if (!m_state) {
        // First poll: allocate state and submit the bind request.
        m_state = std::make_shared<detail::ws::ListenerState>();

        {
            std::lock_guard lk(m_state->bind_mutex);
            m_state->bind_waker = ctx.getWaker();
        }

        // Build comma-separated subprotocol string; empty = accept any.
        std::string proto;
        for (std::size_t i = 0; i < m_subprotocols.size(); ++i) {
            if (i) proto += ',';
            proto += m_subprotocols[i];
        }

        m_io_service->submit(
            std::make_unique<WsBindRequest>(m_state, m_host, m_port, std::move(proto)));
        return PollPending;
    }

    std::lock_guard lk(m_state->bind_mutex);

    if (!m_state->ready) {
        m_state->bind_waker = ctx.getWaker();
        return PollPending;
    }

    if (m_state->bind_error != 0)
        throw std::system_error(
            std::error_code(-m_state->bind_error, std::system_category()),
            "WsListener::bind");

    return WsListener(std::move(m_state), m_io_service);
}

// =============================================================================
// AcceptFuture
// =============================================================================

WsListener::AcceptFuture::AcceptFuture(std::shared_ptr<detail::ws::ListenerState> state,
                                         IoService* io_service)
    : m_state(std::move(state))
    , m_io_service(io_service) {}

PollResult<WsStream> WsListener::AcceptFuture::poll(detail::Context& ctx) {
    if (m_state->closed.load(std::memory_order_acquire))
        throw std::runtime_error("WsListener::accept: listener is closed");

    std::lock_guard lk(m_state->accept_mutex);

    if (!m_state->pending.empty()) {
        auto conn = std::move(m_state->pending.front());
        m_state->pending.pop_front();
        // Construct WsStream from the accepted ConnectionState.
        // WsStream's private constructor is accessible here because AcceptFuture
        // is a nested class of WsListener, and WsStream grants access via friend.
        return WsStream(std::move(conn), m_io_service);
    }

    m_state->accept_waker = ctx.getWaker();
    return PollPending;
}

} // namespace coro
