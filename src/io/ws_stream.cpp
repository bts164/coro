#include <coro/io/ws_stream.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <stdexcept>
#include <system_error>
#include <climits>

namespace coro {


// =============================================================================
// coro::detail::ws — protocol_cb and URL parser
// =============================================================================

namespace detail::ws {

#define LOGSTDOUT(...)
// do { \
//     std::printf("client_protocol_cb:%d -  ", __LINE__); \
//     std::printf(__VA_ARGS__); \
//     std::fflush(stdout); \
// } while (0)

int protocol_cb(lws* wsi, lws_callback_reasons reason,
                void* /*user*/, void* in, std::size_t len) {

    auto* sp = static_cast<std::shared_ptr<ConnectionState>*>(lws_wsi_user(wsi));
    if (!sp) return 0;
    auto& state = **sp;

    switch (reason) {

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        if (state.connect.cancelled.load(std::memory_order_acquire)) {
            // ConnectFuture was dropped — initiate close immediately.
            state.closing.store(true, std::memory_order_release);
            lws_callback_on_writable(wsi);
            LOGSTDOUT("connection established but already cancelled\n");
        } else {
            std::shared_ptr<detail::Waker> waker_to_wake;
            {
                std::lock_guard lk(state.connect.mutex);
                state.connect.complete = true;
                waker_to_wake = state.connect.waker.load();
            }
            LOGSTDOUT("connection established\n");
            if (waker_to_wake) waker_to_wake->wake();
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
        LOGSTDOUT("connection error\n");
        std::shared_ptr<detail::Waker> waker_to_wake;
        {
            std::lock_guard lk(state.connect.mutex);
            state.connect.error = EHOSTUNREACH;  // "No route to host"
            state.connect.complete = true;
            waker_to_wake = state.connect.waker.load();
        }
        if (waker_to_wake) waker_to_wake->wake();
        break;
    }

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        // lws_is_final_fragment / lws_frame_is_binary must be called before the lock
        // since they query lws state only valid during this callback.
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

            // Enforce max_message_size: if the assembled buffer exceeds the limit,
            // surface an EMSGSIZE error to the waiting ReceiveFuture.
            if (state.max_message_size > 0 &&
                    state.receive.buffer.size() > state.max_message_size) {
                LOGSTDOUT("message too large (%zu > %zu), waking with error\n",
                          state.receive.buffer.size(), state.max_message_size);
                state.receive.error    = EMSGSIZE;
                state.receive.complete = true;
                waker_to_wake = state.receive.waker.load();
            } else if (state.frame_mode == WsStream::FrameMode::Partial || final_fragment) {
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

        // Re-arm rx delivery. Without this, lws (especially on the libuv backend)
        // pauses further LWS_CALLBACK_CLIENT_RECEIVE events after the first message
        // until the application explicitly signals it is ready for more data.
        lws_rx_flow_control(wsi, 1);
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        LOGSTDOUT("writeable\n");
        // Close takes priority over pending sends. lws_close_reason() sets the
        // close status code; returning -1 triggers the actual close handshake.
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
                // lws_write requires LWS_PRE bytes of padding before the payload.
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
        LOGSTDOUT("send complete, waking sender\n");
        if (waker_to_wake) waker_to_wake->wake();

        // If more sends are queued, request another WRITEABLE callback.
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

    case LWS_CALLBACK_CLIENT_CLOSED: {
        LOGSTDOUT("connection closed\n");
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

WsStream::WsStream(std::shared_ptr<detail::ws::ConnectionState> state, SingleThreadedUvExecutor* uv_exec)
    : m_state(std::move(state))
    , m_uv_exec(uv_exec) {}

WsStream::WsStream(WsStream&&) noexcept = default;
WsStream& WsStream::operator=(WsStream&&) noexcept = default;

WsStream::~WsStream() {
    if (!m_state) return;
    with_context(*m_uv_exec,
        [](std::shared_ptr<detail::ws::ConnectionState> state) -> Coro<void> {
            if (state->wsi && !state->closed.load()) {
                state->closing.store(true, std::memory_order_release);
                lws_callback_on_writable(state->wsi);
            }
            co_return;
        }(std::move(m_state))
    ).detach();
}

WsStream::ConnectFuture WsStream::connect(std::string url) {
    return connect(std::move(url), Options{});
}

WsStream::ConnectFuture WsStream::connect(std::string url, Options options) {
    return ConnectFuture(std::move(url), std::move(options), &current_uv_executor());
}

WsStream::ReceiveFuture WsStream::receive() {
    return ReceiveFuture(m_state, m_uv_exec);
}

WsStream::SendFuture WsStream::send(std::span<const std::byte> data, OpCode opcode) {
    return SendFuture(m_state, data, opcode, m_uv_exec);
}

WsStream::SendFuture WsStream::send(std::string_view text) {
    return send(std::as_bytes(std::span(text.data(), text.size())), OpCode::Text);
}

// =============================================================================
// ConnectFuture
// =============================================================================

WsStream::ConnectFuture::ConnectFuture(std::string url, Options options,
                                        SingleThreadedUvExecutor* uv_exec)
    : m_url(std::move(url))
    , m_options(std::move(options))
    , m_uv_exec(uv_exec) {}

WsStream::ConnectFuture::~ConnectFuture() {
    if (m_state) {
        std::lock_guard lk(m_state->connect.mutex);
        if (!m_state->connect.complete)
            m_state->connect.cancelled.store(true, std::memory_order_release);
    }
}

PollResult<WsStream> WsStream::ConnectFuture::poll(detail::Context& ctx) {
    if (!m_state) {
        // First poll: allocate connection state and submit the connect request.
        m_state = std::make_shared<detail::ws::ConnectionState>();
        m_state->frame_mode       = m_options.frame_mode;
        m_state->max_message_size = m_options.max_message_size;

        {
            std::lock_guard lk(m_state->connect.mutex);
            m_state->connect.waker.store(ctx.getWaker());
        }

        // Build comma-separated subprotocol string; empty string = no header sent.
        std::string proto;
        for (std::size_t i = 0; i < m_options.subprotocols.size(); ++i) {
            if (i) proto += ',';
            proto += m_options.subprotocols[i];
        }

        lws_context* lws_ctx = m_uv_exec->lws_ctx();
        with_context(*m_uv_exec,
            [](std::shared_ptr<detail::ws::ConnectionState> state,
               std::string url, std::string proto,
               lws_context* lws_ctx) -> Coro<void> {
                using namespace coro::detail::ws;
                if (!lws_ctx) {
                    std::shared_ptr<detail::Waker> w;
                    {
                        std::lock_guard lk(state->connect.mutex);
                        state->connect.error    = ENOTCONN;
                        state->connect.complete = true;
                        w = state->connect.waker.load();
                    }
                    if (w) w->wake();
                    co_return;
                }
                ParsedUrl parsed;
                try {
                    parsed = parse_ws_url(url);
                } catch (...) {
                    std::shared_ptr<detail::Waker> w;
                    {
                        std::lock_guard lk(state->connect.mutex);
                        state->connect.error    = EINVAL;
                        state->connect.complete = true;
                        w = state->connect.waker.load();
                    }
                    if (w) w->wake();
                    co_return;
                }
                auto* sp = new std::shared_ptr<ConnectionState>(state);
                lws_client_connect_info ci{};
                ci.context        = lws_ctx;
                ci.address        = parsed.host.c_str();
                ci.port           = parsed.port;
                ci.path           = parsed.path.c_str();
                ci.ssl_connection = parsed.tls ? LCCSCF_USE_SSL : 0;
                ci.userdata       = sp;
                ci.protocol       = proto.empty() ? nullptr : proto.c_str();
                state->wsi = lws_client_connect_via_info(&ci);
                if (!state->wsi) {
                    delete sp;
                    std::shared_ptr<detail::Waker> w;
                    {
                        std::lock_guard lk(state->connect.mutex);
                        state->connect.error    = ECONNREFUSED;
                        state->connect.complete = true;
                        w = state->connect.waker.load();
                    }
                    if (w) w->wake();
                }
                co_return;
            }(m_state, m_url, std::move(proto), lws_ctx)
        ).detach();
        return PollPending;
    }

    std::lock_guard lk(m_state->connect.mutex);

    if (!m_state->connect.complete) {
        m_state->connect.waker.store(ctx.getWaker());
        return PollPending;
    }

    if (m_state->connect.error != 0)
        throw std::system_error(std::error_code(m_state->connect.error,
                                                std::system_category()),
                                "WsStream::connect");

    return WsStream(std::move(m_state), m_uv_exec);
}

// =============================================================================
// ReceiveFuture
// =============================================================================

WsStream::ReceiveFuture::ReceiveFuture(std::shared_ptr<detail::ws::ConnectionState> state,
                                        SingleThreadedUvExecutor* uv_exec)
    : m_state(std::move(state))
    , m_uv_exec(uv_exec) {}

WsStream::ReceiveFuture::~ReceiveFuture() {
    // Only cancel if this future was never consumed. poll() resets complete=false
    // after consuming the message, so checking complete alone is insufficient —
    // m_done distinguishes "already returned PollReady" from "still in flight".
    if (m_state && !m_done) {
        std::lock_guard lk(m_state->receive.mutex);
        if (!m_state->receive.complete) {
            LOGSTDOUT("receive future dropped, cancelling receive\n");
            m_state->receive.cancelled = true;
        }
    }
}

PollResult<WsStream::Message> WsStream::ReceiveFuture::poll(detail::Context& ctx) {
    std::lock_guard lk(m_state->receive.mutex);

    if (m_state->receive.complete) {
        int    error    = m_state->receive.error;
        auto   buf      = std::move(m_state->receive.buffer);
        bool   is_text  = m_state->receive.is_text;
        bool   is_final = m_state->receive.is_final;

        // Reset sub-state atomically under the lock so protocol_cb cannot
        // append to the buffer or set complete again until we're done.
        m_state->receive.buffer    = {};
        m_state->receive.complete  = false;
        m_state->receive.cancelled = false;
        m_state->receive.error     = 0;

        m_done = true;

        if (error != 0)
            throw std::system_error(std::error_code(error, std::system_category()),
                                    "WsStream::receive");

        return Message{std::move(buf), is_text, is_final};
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
                                   SingleThreadedUvExecutor*                     uv_exec)
    : m_state(std::move(state))
    , m_uv_exec(uv_exec) {
    m_sub_state = std::make_shared<detail::ws::SendSubState>();
    m_sub_state->data   = data;
    m_sub_state->opcode = opcode;
}

WsStream::SendFuture::~SendFuture() {
    if (m_sub_state) {
        std::lock_guard lk(m_sub_state->mutex);
        if (!m_sub_state->complete)
            m_sub_state->cancelled = true;
    }
}

PollResult<void> WsStream::SendFuture::poll(detail::Context& ctx) {
    std::lock_guard lk(m_sub_state->mutex);

    if (m_sub_state->complete) {
        if (m_sub_state->error != 0)
            throw std::system_error(std::error_code(-m_sub_state->error,
                                                    std::system_category()),
                                    "WsStream::send");
        return PollReady;
    }

    if (m_state->closed.load(std::memory_order_acquire))
        throw std::runtime_error("WsStream::send: connection closed");

    m_sub_state->waker.store(ctx.getWaker());

    if (!m_started) {
        m_started = true;
        // Enqueue the sub-state and request a WRITEABLE callback.
        {
            std::lock_guard lk2(m_state->send_queue_mutex);
            m_state->send_queue.push_back(m_sub_state);
        }
        with_context(*m_uv_exec,
            [](std::shared_ptr<detail::ws::ConnectionState> state) -> Coro<void> {
                if (state->wsi && !state->closed.load())
                    lws_callback_on_writable(state->wsi);
                co_return;
            }(m_state)
        ).detach();
    }

    return PollPending;
}

} // namespace coro
