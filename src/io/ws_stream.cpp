// WsStream on libwebsockets, with every lws call on the lws service thread
// (ws_service.h). See doc/design/websocket_stream.md.

#include <coro/io/ws_stream.h>
#include <coro/io/lookup_host.h>
#include <coro/io/socket_address.h>
#include "ws_service.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <algorithm>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <span>
#include <utility>
#include <variant>

namespace coro {

// =============================================================================
// coro::detail::ws — callbacks and URL parser
// =============================================================================

namespace detail::ws {

int protocol_cb(lws* wsi, lws_callback_reasons reason,
                void* user, void* in, std::size_t len) {
    // The wsi user pointer: the ConnectionState the connect command passed as
    // userdata. Cleared before close_connection() drops the state, so callbacks after
    // that (and the context-level ones, which have no connection) see null.
    auto* state = static_cast<ConnectionState*>(user);
    if (!state) return 0;

    switch (reason) {

    case LWS_CALLBACK_CLIENT_ESTABLISHED: {
        state->wsi = wsi;
        bool cancelled;
        Weak<Waker> waker;
        {
            std::lock_guard lk(state->connect.mutex);
            cancelled = state->connect.cancelled;
            if (!cancelled) {
                state->connect.complete = true;
                waker = state->connect.waker;
            }
        }
        if (cancelled) {
            // The ConnectAttempt was dropped: nobody will take this stream, so close it.
            state->closing = true;
            lws_callback_on_writable(wsi);
        } else {
            wake(waker);
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
        // lws reports only text (e.g. "connect failed" or an HTTP status from the
        // upgrade), so the errno is a stand-in for "this address didn't work".
        std::string reason_text = in ? std::string(static_cast<const char*>(in), len)
                                     : std::string("connection failed");
        lws_set_wsi_user(wsi, nullptr);
        close_connection(*state, ECONNREFUSED, std::move(reason_text));
        break;
    }

    case LWS_CALLBACK_CLIENT_RECEIVE:
        // lws_is_final_fragment / lws_frame_is_binary are only valid during this callback.
        on_receive(*state, std::span(static_cast<const std::byte*>(in), len),
                   lws_frame_is_binary(wsi) == 0, lws_is_final_fragment(wsi));
        // Keep rx enabled. lws can pause RECEIVE after a message until the application
        // says it is ready for more; we always are, since ready is a queue.
        lws_rx_flow_control(wsi, 1);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        return on_writeable(*state, wsi);

    // CLOSED for an established connection. WSI_DESTROY catches a wsi that goes
    // without either CLOSED or CONNECTION_ERROR, e.g. mid-handshake when its context
    // is destroyed.
    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_WSI_DESTROY:
        lws_set_wsi_user(wsi, nullptr);
        close_connection(*state, ECONNABORTED, "connection closed during the handshake");
        break;

    default:
        break;
    }
    return 0;
}

void on_receive(ConnectionState& state, std::span<const std::byte> fragment,
                bool is_text, bool is_final_fragment) {
    auto& rx = state.receive;
    Weak<Waker> waker;
    {
        std::lock_guard lk(rx.mutex);
        if (!rx.discarding) {
            rx.message_size += fragment.size();
            if (state.max_message_size > 0 && rx.message_size > state.max_message_size) {
                // Surface one EMSGSIZE in the message's place and drop the rest of it, so
                // the connection stays usable for the messages after it.
                rx.buffer.clear();
                rx.ready.push_back({.is_text = is_text, .is_final = true, .error = EMSGSIZE});
                rx.discarding = true;
                waker = rx.waker;
            } else {
                rx.buffer.insert(rx.buffer.end(), fragment.begin(), fragment.end());
                if (state.frame_mode == WsStream::FrameMode::Partial || is_final_fragment) {
                    rx.ready.push_back({std::exchange(rx.buffer, {}), is_text, is_final_fragment, 0});
                    waker = rx.waker;
                }
            }
        }
        if (is_final_fragment) {
            rx.message_size = 0;
            rx.discarding   = false;
        }
    }
    wake(waker);
}

int on_writeable(ConnectionState& state, lws* wsi) {
    // Close takes priority over pending sends; close_connection() fails those.
    // lws_close_reason() sets the status code; returning -1 starts the close.
    if (state.closing) {
        lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
        return -1;
    }

    std::shared_ptr<SendSubState> sub;
    {
        std::lock_guard lk(state.send_queue_mutex);
        if (state.send_queue.empty()) return 0;
        sub = std::move(state.send_queue.front());
        state.send_queue.pop_front();
    }

    Weak<Waker> waker;
    {
        std::lock_guard lk(sub->mutex);
        if (!sub->cancelled) {
            // Copied under the lock: once it's released, ~SendFuture may run and the
            // caller may free the buffer. lws_write needs LWS_PRE bytes of headroom.
            std::vector<std::byte> buf(LWS_PRE + sub->data.size());
            std::copy(sub->data.begin(), sub->data.end(), buf.begin() + LWS_PRE);
            const auto flags = sub->opcode == WsStream::OpCode::Binary ? LWS_WRITE_BINARY
                                                                       : LWS_WRITE_TEXT;
            const int r = lws_write(wsi, reinterpret_cast<unsigned char*>(buf.data() + LWS_PRE),
                                    sub->data.size(), flags);
            sub->error = r < 0 ? EIO : 0;
            sub->data  = {};
        }
        sub->complete = true;
        waker = sub->waker;
    }
    wake(waker);

    std::lock_guard lk(state.send_queue_mutex);
    if (!state.send_queue.empty()) lws_callback_on_writable(wsi);
    return 0;
}

void close_connection(ConnectionState& state, int connect_error, std::string reason) {
    state.wsi = nullptr;

    Weak<Waker> connect_waker;
    {
        std::lock_guard lk(state.connect.mutex);
        if (!state.connect.complete) {
            state.connect.complete = true;
            state.connect.error    = connect_error;
            state.connect.reason   = std::move(reason);
            connect_waker          = state.connect.waker;
        }
    }

    // Set before taking the receive and send-queue mutexes. ReceiveFuture and
    // SendFuture check it under those mutexes, so each either sees it or has already
    // stored its waker / queued its send, which is woken / failed below.
    state.closed.store(true, std::memory_order_release);

    Weak<Waker> receive_waker;
    {
        std::lock_guard lk(state.receive.mutex);
        receive_waker = state.receive.waker;
    }

    std::vector<Weak<Waker>> send_wakers;
    {
        std::lock_guard lk(state.send_queue_mutex);
        for (auto& sub : state.send_queue) {
            std::lock_guard slk(sub->mutex);
            sub->error    = ENOTCONN;
            sub->complete = true;
            send_wakers.push_back(sub->waker);
        }
        state.send_queue.clear();
    }

    wake(connect_waker);
    wake(receive_waker);
    for (auto& waker : send_wakers) wake(waker);

    // Last: this may be the final reference, freeing `state`.
    auto self = std::move(state.self);
}

void post_close(LwsService& service, std::shared_ptr<ConnectionState> state) {
    service.post([state = std::move(state)] {
        // wsi is null once lws has reported the connection gone.
        if (!state->wsi) return;
        state->closing = true;
        lws_callback_on_writable(state->wsi);
    });
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

std::string numeric_host(const SocketAddress& addr) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (const auto* v4 = std::get_if<Ipv4Address>(&addr.address))
        inet_ntop(AF_INET, v4->octets.data(), buf, sizeof(buf));
    else
        inet_ntop(AF_INET6, std::get<Ipv6Address>(addr.address).octets.data(), buf, sizeof(buf));
    return buf;
}

} // namespace detail::ws

// =============================================================================
// ConnectAttempt — one lws_client_connect_via_info() to one address
// =============================================================================

/// Future<WsStream> for a single address. On first poll, posts the connect; the
/// client callback completes it at ESTABLISHED or on failure.
///
/// Dropping it before ESTABLISHED sets `cancelled`: the command then skips the
/// connect, or ESTABLISHED closes the connection. Dropping it after ESTABLISHED, but
/// before the poll that takes the stream, closes the connection itself.
class WsStream::ConnectAttempt {
public:
    using OutputType = WsStream;

    ConnectAttempt(std::shared_ptr<detail::ws::LwsService> service, std::string address,
                   const detail::ws::ParsedUrl& url, std::string protocols,
                   const Options& options)
        : m_service(std::move(service))
        , m_state(std::make_shared<detail::ws::ConnectionState>())
        , m_address(std::move(address))
        , m_url(url)
        , m_protocols(std::move(protocols)) {
        m_state->frame_mode       = options.frame_mode;
        m_state->max_message_size = options.max_message_size;
    }

    ConnectAttempt(ConnectAttempt&&) noexcept            = default;
    ConnectAttempt& operator=(ConnectAttempt&&) noexcept = delete;

    ~ConnectAttempt() {
        if (!m_state) return;   // moved from, or the stream was taken
        bool established;
        {
            std::lock_guard lk(m_state->connect.mutex);
            if (!m_state->connect.complete) m_state->connect.cancelled = true;
            established = m_state->connect.complete && m_state->connect.error == 0;
        }
        if (established) detail::ws::post_close(*m_service, std::move(m_state));
    }

    PollResult<WsStream> poll(detail::Context& ctx) {
        if (!m_started) {
            m_started = true;
            {
                std::lock_guard lk(m_state->connect.mutex);
                m_state->connect.waker = ctx.get_weak_waker();
            }
            post_connect();
            return PollPending;
        }

        std::lock_guard lk(m_state->connect.mutex);
        if (!m_state->connect.complete) {
            m_state->connect.waker = ctx.get_weak_waker();
            return PollPending;
        }
        if (m_state->connect.error != 0)
            throw std::system_error(m_state->connect.error, std::system_category(),
                                    "WsStream::connect: " + m_state->connect.reason);
        return WsStream(std::move(m_state), m_service);
    }

private:
    void post_connect() {
        // Captures the raw context, never the service: see LwsService::post().
        m_service->post([state = m_state, ctx = m_service->context(), address = m_address,
                         url = m_url, protocols = m_protocols] {
            {
                std::lock_guard lk(state->connect.mutex);
                if (state->connect.cancelled) return;
            }
            lws_client_connect_info ci{};
            ci.context        = ctx;
            ci.address        = address.c_str();     // numeric: lws resolves synchronously
            ci.host           = url.host.c_str();    // Host header and TLS SNI
            ci.port           = url.port;
            ci.path           = url.path.c_str();
            ci.ssl_connection = url.tls ? LCCSCF_USE_SSL : 0;
            ci.protocol       = protocols.empty() ? nullptr : protocols.c_str();
            ci.userdata       = state.get();
            state->self = state;
            // Not kept: ESTABLISHED records the wsi. A failure may already have been
            // reported through CONNECTION_ERROR before this returns, and the wsi
            // freed, so the return value is only a success flag.
            if (!lws_client_connect_via_info(&ci))
                detail::ws::close_connection(*state, ECONNREFUSED,
                                             "lws_client_connect_via_info failed");
        });
    }

    std::shared_ptr<detail::ws::LwsService>      m_service;
    std::shared_ptr<detail::ws::ConnectionState> m_state;
    std::string                                  m_address;
    detail::ws::ParsedUrl                        m_url;
    std::string                                  m_protocols;
    bool                                         m_started = false;
};

// =============================================================================
// WsStream
// =============================================================================

WsStream::WsStream(std::shared_ptr<detail::ws::ConnectionState> state,
                   std::shared_ptr<detail::ws::LwsService>      service)
    : m_state(std::move(state))
    , m_service(std::move(service)) {}

WsStream::WsStream(WsStream&&) noexcept = default;

WsStream& WsStream::operator=(WsStream&& other) noexcept {
    if (this != &other) {
        // The connection being replaced is closed, as ~WsStream would; dropping its
        // state alone would leave it open, held by its own self reference.
        if (m_state) detail::ws::post_close(*m_service, std::move(m_state));
        m_state   = std::move(other.m_state);
        m_service = std::move(other.m_service);
    }
    return *this;
}

WsStream::~WsStream() {
    if (m_state) detail::ws::post_close(*m_service, std::move(m_state));
    // m_service is released after this body; if it's the last reference, that joins
    // the service thread, which runs the close first.
}

Coro<WsStream> WsStream::connect(std::string url) {
    return connect(std::move(url), Options{});
}

Coro<WsStream> WsStream::connect(std::string url, Options options) {
    const detail::ws::ParsedUrl parsed = detail::ws::parse_ws_url(url);
    const std::vector<SocketAddress> peers = co_await lookup_host(parsed.host, parsed.port);

    // Comma-separated Sec-WebSocket-Protocol; empty sends no header.
    std::string protocols;
    for (const auto& p : options.subprotocols) {
        if (!protocols.empty()) protocols += ',';
        protocols += p;
    }

    auto service = detail::ws::LwsService::client();
    // Each address in the resolver's order; the first to complete the handshake wins,
    // else the last failure is thrown (as TcpStream::connect does).
    std::exception_ptr last_error;
    for (const SocketAddress& peer : peers) {
#if !defined(LWS_WITH_IPV6)
        if (std::holds_alternative<Ipv6Address>(peer.address)) {
            last_error = std::make_exception_ptr(std::system_error(
                EAFNOSUPPORT, std::system_category(),
                "WsStream::connect: libwebsockets was built without IPv6"));
            continue;
        }
#endif
        try {
            co_return co_await ConnectAttempt(service, detail::ws::numeric_host(peer),
                                              parsed, protocols, options);
        } catch (const std::system_error&) {
            last_error = std::current_exception();
        }
    }
    std::rethrow_exception(last_error);   // non-null: lookup_host never returns empty
}

WsStream::ReceiveFuture WsStream::receive() {
    if (!m_state) throw std::logic_error("WsStream::receive: moved-from WsStream");
    return ReceiveFuture(m_state, m_service);
}

WsStream::SendFuture WsStream::send(std::span<const std::byte> data, OpCode opcode) {
    if (!m_state) throw std::logic_error("WsStream::send: moved-from WsStream");
    return SendFuture(m_state, data, opcode, m_service);
}

WsStream::SendFuture WsStream::send(std::string_view text) {
    return send(std::as_bytes(std::span(text.data(), text.size())), OpCode::Text);
}

// =============================================================================
// ReceiveFuture
// =============================================================================

WsStream::ReceiveFuture::ReceiveFuture(std::shared_ptr<detail::ws::ConnectionState> state,
                                       std::shared_ptr<detail::ws::LwsService>      service)
    : m_state(std::move(state))
    , m_service(std::move(service)) {}

PollResult<WsStream::Message> WsStream::ReceiveFuture::poll(detail::Context& ctx) {
    std::lock_guard lk(m_state->receive.mutex);

    auto& ready = m_state->receive.ready;
    if (!ready.empty()) {
        detail::ws::ReceivedMessage msg = std::move(ready.front());
        ready.pop_front();
        if (msg.error != 0)
            throw std::system_error(std::error_code(msg.error, std::system_category()),
                                    "WsStream::receive");
        return Message{std::move(msg.data), msg.is_text, msg.is_final};
    }

    // Checked after the queue, so messages that arrived before the close are still
    // delivered. close_connection() sets closed before taking this mutex to read the
    // waker, so either this sees closed or it sees the waker stored below.
    if (m_state->closed.load(std::memory_order_acquire))
        throw std::runtime_error("WsStream::receive: connection closed");

    m_state->receive.waker = ctx.get_weak_waker();
    return PollPending;
}

// =============================================================================
// SendFuture
// =============================================================================

WsStream::SendFuture::SendFuture(std::shared_ptr<detail::ws::ConnectionState> state,
                                 std::span<const std::byte>                    data,
                                 OpCode                                        opcode,
                                 std::shared_ptr<detail::ws::LwsService>       service)
    : m_state(std::move(state))
    , m_sub_state(std::make_shared<detail::ws::SendSubState>())
    , m_service(std::move(service)) {
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
    {
        std::lock_guard lk(m_sub_state->mutex);
        if (m_sub_state->complete) {
            if (m_sub_state->error != 0)
                throw std::system_error(m_sub_state->error, std::system_category(),
                                        "WsStream::send");
            return PollReady;
        }
        m_sub_state->waker = ctx.get_weak_waker();
    }

    if (!m_started) {
        // Not under the sub-state mutex: close_connection() takes the queue mutex and
        // then each sub-state's, so taking them in the other order could deadlock.
        {
            std::lock_guard lk(m_state->send_queue_mutex);
            // Under the queue mutex: close_connection() sets closed before draining
            // the queue, so a send queued here is always either written or failed.
            if (m_state->closed.load(std::memory_order_acquire))
                throw std::runtime_error("WsStream::send: connection closed");
            m_state->send_queue.push_back(m_sub_state);
        }
        m_started = true;
        m_service->post([state = m_state] {
            if (state->wsi) lws_callback_on_writable(state->wsi);
        });
    }
    return PollPending;
}

} // namespace coro
