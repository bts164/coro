#include <coro/io/ws_stream.h>
#include <stdexcept>
#include <system_error>
#include <climits>

namespace coro {

// =============================================================================
// IoRequest subtypes — defined here (not in the header) because they are purely
// internal. IoService has no knowledge of these types.
// =============================================================================

namespace {

using namespace coro::detail::ws;

// ---------------------------------------------------------------------------
// WsConnectRequest
//
// Submitted by ConnectFuture on first poll. Runs on the I/O thread: parses the
// URL, stores a heap-allocated shared_ptr<ConnectionState> as lws per-session
// user data, then calls lws_client_connect_via_info(). From that point on,
// protocol_cb recovers the ConnectionState via lws_wsi_user().
// ---------------------------------------------------------------------------
struct WsConnectRequest : coro::IoRequest {
    std::shared_ptr<ConnectionState> state;
    std::string                      url;
    std::string                      subprotocol_str;  // comma-joined; empty = no subprotocol header
    lws_context*                     lws_ctx;

    WsConnectRequest(std::shared_ptr<ConnectionState> s,
                     std::string                      u,
                     std::string                      proto,
                     lws_context*                     ctx)
        : state(std::move(s)), url(std::move(u)), subprotocol_str(std::move(proto)), lws_ctx(ctx) {}

    void execute(uv_loop_t* /*loop*/) override {
        if (!lws_ctx) {
            state->connect.error = -1;
            state->connect.complete.store(true, std::memory_order_release);
            if (auto w = state->connect.waker.load()) w->wake();
            return;
        }

        ParsedUrl parsed;
        try {
            parsed = parse_ws_url(url);
        } catch (...) {
            state->connect.error = -1;
            state->connect.complete.store(true, std::memory_order_release);
            if (auto w = state->connect.waker.load()) w->wake();
            return;
        }

        // Heap-allocate a shared_ptr wrapper stored as lws per-session user data.
        // protocol_cb recovers it via lws_wsi_user(); LWS_CALLBACK_CLIENT_CLOSED
        // deletes the wrapper, decrementing the ref count.
        auto* sp = new std::shared_ptr<ConnectionState>(state);

        lws_client_connect_info ci{};
        ci.context        = lws_ctx;
        ci.address        = parsed.host.c_str();
        ci.port           = parsed.port;
        ci.path           = parsed.path.c_str();
        ci.ssl_connection = parsed.tls ? LCCSCF_USE_SSL : 0;
        ci.userdata       = sp;
        // nullptr omits the Sec-WebSocket-Protocol request header entirely,
        // making WsStream connect to any server regardless of subprotocol.
        ci.protocol       = subprotocol_str.empty() ? nullptr : subprotocol_str.c_str();

        state->wsi = lws_client_connect_via_info(&ci);
        if (!state->wsi) {
            delete sp;
            state->connect.error = -1;
            state->connect.complete.store(true, std::memory_order_release);
            if (auto w = state->connect.waker.load()) w->wake();
        }
    }
};

// ---------------------------------------------------------------------------
// WsWritableRequest
//
// Submitted by SendFuture after enqueuing its SendSubState. Calls
// lws_callback_on_writable() on the I/O thread so lws schedules a
// LWS_CALLBACK_CLIENT_WRITEABLE event.
// ---------------------------------------------------------------------------
struct WsWritableRequest : coro::IoRequest {
    std::shared_ptr<ConnectionState> state;
    explicit WsWritableRequest(std::shared_ptr<ConnectionState> s) : state(std::move(s)) {}

    void execute(uv_loop_t* /*loop*/) override {
        if (state->wsi && !state->closed.load())
            lws_callback_on_writable(state->wsi);
    }
};

// ---------------------------------------------------------------------------
// WsCloseRequest
//
// Submitted by WsStream::~WsStream(). Initiates a graceful WebSocket close
// (sends a Close frame) from the I/O thread.
// ---------------------------------------------------------------------------
struct WsCloseRequest : coro::IoRequest {
    std::shared_ptr<ConnectionState> state;
    explicit WsCloseRequest(std::shared_ptr<ConnectionState> s) : state(std::move(s)) {}

    void execute(uv_loop_t* /*loop*/) override {
        if (state->wsi && !state->closed.load()) {
            lws_close_reason(state->wsi, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
            // Setting the close reason causes lws to send the Close frame on the
            // next writeable callback. Request one now.
            lws_callback_on_writable(state->wsi);
        }
    }
};

} // anonymous namespace

// =============================================================================
// coro::detail::ws — protocol_cb and URL parser
// =============================================================================

namespace detail::ws {

int protocol_cb(lws* wsi, lws_callback_reasons reason,
                void* /*user*/, void* in, std::size_t len) {

    auto* sp = static_cast<std::shared_ptr<ConnectionState>*>(lws_wsi_user(wsi));
    if (!sp) return 0;
    auto& state = **sp;

    switch (reason) {

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        if (state.connect.cancelled.load(std::memory_order_acquire)) {
            // ConnectFuture was dropped — initiate close immediately.
            lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
            lws_callback_on_writable(wsi);
        } else {
            state.connect.complete.store(true, std::memory_order_release);
            if (auto w = state.connect.waker.load()) w->wake();
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        state.connect.error = -1;  // TODO: surface lws error string
        state.connect.complete.store(true, std::memory_order_release);
        if (auto w = state.connect.waker.load()) w->wake();
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        // lws_is_final_fragment / lws_frame_is_binary must be called before the lock
        // since they query lws state only valid during this callback.
        bool final_fragment = lws_is_final_fragment(wsi);
        bool is_text        = (lws_frame_is_binary(wsi) == 0);
        auto* bytes         = static_cast<const std::byte*>(in);

        std::printf("protocol_cb: received %zu bytes (final=%d, text=%d)\n", len, final_fragment, is_text);
        std::shared_ptr<detail::Waker> waker_to_wake;
        {
            std::lock_guard lk(state.receive.mutex);
            if (state.receive.cancelled) {
                state.receive.buffer.clear();
                break;
            }
            state.receive.buffer.insert(state.receive.buffer.end(), bytes, bytes + len);

            if (state.frame_mode == WsStream::FrameMode::Partial || final_fragment) {
                state.receive.is_text  = is_text;
                state.receive.is_final = final_fragment;
                state.receive.complete = true;
                waker_to_wake = state.receive.waker.load();
            }
        }
        if (waker_to_wake) waker_to_wake->wake();
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        std::shared_ptr<SendSubState> sub;
        {
            std::lock_guard lk(state.send_queue_mutex);
            if (state.send_queue.empty()) break;
            sub = state.send_queue.front();
            state.send_queue.pop_front();
        }

        if (!sub->cancelled.load(std::memory_order_acquire)) {
            // lws_write requires LWS_PRE bytes of padding before the payload.
            std::vector<std::byte> buf(LWS_PRE + sub->data.size());
            std::copy(sub->data.begin(), sub->data.end(), buf.begin() + LWS_PRE);
            int flags = (sub->opcode == WsStream::OpCode::Binary)
                            ? LWS_WRITE_BINARY : LWS_WRITE_TEXT;
            int r = lws_write(wsi,
                              reinterpret_cast<unsigned char*>(buf.data() + LWS_PRE),
                              sub->data.size(),
                              static_cast<lws_write_protocol>(flags));
            sub->error = (r < 0) ? r : 0;
            sub->data  = {};
        }
        sub->complete.store(true, std::memory_order_release);
        if (auto w = sub->waker.load()) w->wake();

        // If more sends are queued, request another WRITEABLE callback.
        {
            std::lock_guard lk(state.send_queue_mutex);
            if (!state.send_queue.empty())
                lws_callback_on_writable(wsi);
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_CLOSED: {
        state.wsi = nullptr;
        state.closed.store(true, std::memory_order_release);
        // Wake any receive future blocked on this connection.
        std::shared_ptr<detail::Waker> recv_waker;
        {
            std::lock_guard lk(state.receive.mutex);
            recv_waker = state.receive.waker.load();
        }
        if (recv_waker) recv_waker->wake();
        // Wake and error all queued sends.
        {
            std::lock_guard lk(state.send_queue_mutex);
            for (auto& sub : state.send_queue) {
                sub->error = -1;
                sub->complete.store(true, std::memory_order_release);
                if (auto w = sub->waker.load()) w->wake();
            }
            state.send_queue.clear();
        }
        // Delete the heap-allocated shared_ptr wrapper — may free ConnectionState.
        delete sp;
        break;
    }

    default:
        break;
    }
    return 0;
}

ParsedUrl parse_ws_url(std::string_view url) {
    ParsedUrl result;
    std::string_view rest;

    if (url.starts_with("wss://")) {
        result.tls = true;
        result.port = 443;
        rest = url.substr(6);
    } else if (url.starts_with("ws://")) {
        result.tls = false;
        result.port = 80;
        rest = url.substr(5);
    } else {
        throw std::invalid_argument("WsStream: URL must start with ws:// or wss://");
    }

    auto path_pos = rest.find('/');
    std::string_view authority = (path_pos == std::string_view::npos)
                                     ? rest : rest.substr(0, path_pos);
    result.path = (path_pos == std::string_view::npos) ? "/" : std::string(rest.substr(path_pos));

    auto colon = authority.find(':');
    if (colon != std::string_view::npos) {
        result.host = std::string(authority.substr(0, colon));
        auto port_str = authority.substr(colon + 1);
        int port = 0;
        for (char c : port_str) {
            if (c < '0' || c > '9')
                throw std::invalid_argument("WsStream: invalid port in URL");
            port = port * 10 + (c - '0');
        }
        if (port < 1 || port > 65535)
            throw std::invalid_argument("WsStream: port out of range");
        result.port = static_cast<uint16_t>(port);
    } else {
        result.host = std::string(authority);
    }

    if (result.host.empty())
        throw std::invalid_argument("WsStream: missing host in URL");

    return result;
}

} // namespace detail::ws

// =============================================================================
// WsStream
// =============================================================================

WsStream::WsStream(std::shared_ptr<detail::ws::ConnectionState> state, IoService* io_service)
    : m_state(std::move(state))
    , m_io_service(io_service) {}

WsStream::WsStream(WsStream&&) noexcept = default;
WsStream& WsStream::operator=(WsStream&&) noexcept = default;

WsStream::~WsStream() {
    if (m_state)
        m_io_service->submit(std::make_unique<WsCloseRequest>(std::move(m_state)));
}

WsStream::ConnectFuture WsStream::connect(std::string url, FrameMode frame_mode,
                                           std::vector<std::string> subprotocols) {
    return ConnectFuture(std::move(url), frame_mode, std::move(subprotocols),
                         &current_io_service());
}

WsStream::ReceiveFuture WsStream::receive() {
    return ReceiveFuture(m_state, m_io_service);
}

WsStream::SendFuture WsStream::send(std::span<const std::byte> data, OpCode opcode) {
    return SendFuture(m_state, data, opcode, m_io_service);
}

WsStream::SendFuture WsStream::send(std::string_view text) {
    return send(std::as_bytes(std::span(text.data(), text.size())), OpCode::Text);
}

// =============================================================================
// ConnectFuture
// =============================================================================

WsStream::ConnectFuture::ConnectFuture(std::string url, FrameMode frame_mode,
                                        std::vector<std::string> subprotocols,
                                        IoService* io_service)
    : m_url(std::move(url))
    , m_frame_mode(frame_mode)
    , m_subprotocols(std::move(subprotocols))
    , m_io_service(io_service) {}

WsStream::ConnectFuture::~ConnectFuture() {
    if (m_state && !m_state->connect.complete.load(std::memory_order_acquire))
        m_state->connect.cancelled.store(true, std::memory_order_release);
}

PollResult<WsStream> WsStream::ConnectFuture::poll(detail::Context& ctx) {
    if (!m_state) {
        // First poll: allocate connection state and submit the connect request.
        m_state = std::make_shared<detail::ws::ConnectionState>();
        m_state->frame_mode = m_frame_mode;
        m_state->connect.waker.store(ctx.getWaker());

        // Build comma-separated subprotocol string; empty string = no header sent.
        std::string proto;
        for (std::size_t i = 0; i < m_subprotocols.size(); ++i) {
            if (i) proto += ',';
            proto += m_subprotocols[i];
        }

        m_io_service->submit(std::make_unique<WsConnectRequest>(
            m_state, m_url, std::move(proto), m_io_service->lws_ctx()));
        return PollPending;
    }

    if (!m_state->connect.complete.load(std::memory_order_acquire)) {
        m_state->connect.waker.store(ctx.getWaker());
        return PollPending;
    }

    if (m_state->connect.error != 0)
        throw std::system_error(std::error_code(-m_state->connect.error,
                                                std::system_category()),
                                "WsStream::connect");

    return WsStream(std::move(m_state), m_io_service);
}

// =============================================================================
// ReceiveFuture
// =============================================================================

WsStream::ReceiveFuture::ReceiveFuture(std::shared_ptr<detail::ws::ConnectionState> state,
                                        IoService* io_service)
    : m_state(std::move(state))
    , m_io_service(io_service) {}

WsStream::ReceiveFuture::~ReceiveFuture() {
    if (m_state) {
        std::lock_guard lk(m_state->receive.mutex);
        if (!m_state->receive.complete)
            m_state->receive.cancelled = true;
    }
}

PollResult<WsStream::Message> WsStream::ReceiveFuture::poll(detail::Context& ctx) {
    std::lock_guard lk(m_state->receive.mutex);

    if (m_state->receive.complete) {
        Message msg;
        msg.data     = std::move(m_state->receive.buffer);
        msg.is_text  = m_state->receive.is_text;
        msg.is_final = m_state->receive.is_final;

        // Reset sub-state atomically under the lock so protocol_cb cannot
        // append to the buffer or set complete again until we're done.
        m_state->receive.buffer    = {};
        m_state->receive.complete  = false;
        m_state->receive.cancelled = false;

        return msg;
    }

    if (m_state->closed.load(std::memory_order_acquire))
        throw std::runtime_error("WsStream::receive: connection closed");

    m_state->receive.waker.store(ctx.getWaker());
    return PollPending;
}

// =============================================================================
// SendFuture
// =============================================================================

WsStream::SendFuture::SendFuture(std::shared_ptr<detail::ws::ConnectionState> state,
                                   std::span<const std::byte>                    data,
                                   OpCode                                        opcode,
                                   IoService*                                    io_service)
    : m_state(std::move(state))
    , m_io_service(io_service) {
    m_sub_state = std::make_shared<detail::ws::SendSubState>();
    m_sub_state->data   = data;
    m_sub_state->opcode = opcode;
}

WsStream::SendFuture::~SendFuture() {
    if (m_sub_state && !m_sub_state->complete.load(std::memory_order_acquire))
        m_sub_state->cancelled.store(true, std::memory_order_release);
}

PollResult<void> WsStream::SendFuture::poll(detail::Context& ctx) {
    if (m_state->closed.load(std::memory_order_acquire) &&
        !m_sub_state->complete.load(std::memory_order_acquire))
        throw std::runtime_error("WsStream::send: connection closed");

    if (m_sub_state->complete.load(std::memory_order_acquire)) {
        if (m_sub_state->error != 0)
            throw std::system_error(std::error_code(-m_sub_state->error,
                                                    std::system_category()),
                                    "WsStream::send");
        return PollReady;
    }

    m_sub_state->waker.store(ctx.getWaker());

    if (!m_started) {
        m_started = true;
        // Enqueue the sub-state and request a WRITEABLE callback.
        {
            std::lock_guard lk(m_state->send_queue_mutex);
            m_state->send_queue.push_back(m_sub_state);
        }
        m_io_service->submit(std::make_unique<WsWritableRequest>(m_state));
    }

    return PollPending;
}

} // namespace coro
