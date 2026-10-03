// WsListener on libwebsockets, with its own lws context and service thread
// (ws_service.h). See doc/design/websocket_stream.md.

#include <coro/io/ws_listener.h>
#include <coro/io/lookup_host.h>
#include <coro/io/socket_address.h>
#include <coro/detail/sys/tcp.h>
#include "ws_service.h"
#include <cctype>
#include <cerrno>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace coro {

// =============================================================================
// WsUpgradeRequest
// =============================================================================

std::string WsUpgradeRequest::path() const {
    char buf[1024] = {};
    int n = lws_hdr_copy(m_wsi, buf, static_cast<int>(sizeof(buf)) - 1, WSI_TOKEN_GET_URI);
    return (n > 0) ? std::string(buf, static_cast<std::size_t>(n)) : std::string{};
}

std::string WsUpgradeRequest::header(std::string_view name) const {
    // Map well-known header names (lowercase) to lws token IDs.
    static const struct { const char* name; lws_token_indexes token; } known[] = {
        {"authorization",    WSI_TOKEN_HTTP_AUTHORIZATION},
        {"content-type",     WSI_TOKEN_HTTP_CONTENT_TYPE},
        {"host",             WSI_TOKEN_HOST},
        {"origin",           WSI_TOKEN_ORIGIN},
        {"user-agent",       WSI_TOKEN_HTTP_USER_AGENT},
        {"cookie",           WSI_TOKEN_HTTP_COOKIE},
        {"sec-websocket-protocol", WSI_TOKEN_PROTOCOL},
    };
    std::string lower(name);
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    char buf[4096] = {};
    for (auto& e : known) {
        if (lower == e.name) {
            int n = lws_hdr_copy(m_wsi, buf, static_cast<int>(sizeof(buf)) - 1, e.token);
            return (n > 0) ? std::string(buf, static_cast<std::size_t>(n)) : std::string{};
        }
    }
    // Fall back to custom (non-standard) header lookup.
    // lws_hdr_custom_copy expects the name with a trailing colon.
    std::string name_colon(name);
    if (name_colon.empty() || name_colon.back() != ':')
        name_colon += ':';
    int n = lws_hdr_custom_copy(m_wsi, buf, static_cast<int>(sizeof(buf)) - 1,
                                 name_colon.c_str(), static_cast<int>(name_colon.size()));
    return (n > 0) ? std::string(buf, static_cast<std::size_t>(n)) : std::string{};
}

std::vector<std::string> WsUpgradeRequest::offered_subprotocols() const {
    char buf[1024] = {};
    int n = lws_hdr_copy(m_wsi, buf, static_cast<int>(sizeof(buf)) - 1, WSI_TOKEN_PROTOCOL);
    if (n <= 0) return {};
    std::vector<std::string> result;
    std::string_view sv(buf, static_cast<std::size_t>(n));
    while (!sv.empty()) {
        auto comma = sv.find(',');
        auto tok   = (comma == std::string_view::npos) ? sv : sv.substr(0, comma);
        auto start = tok.find_first_not_of(" \t");
        auto end   = tok.find_last_not_of(" \t");
        if (start != std::string_view::npos)
            result.emplace_back(tok.substr(start, end - start + 1));
        if (comma == std::string_view::npos) break;
        sv = sv.substr(comma + 1);
    }
    return result;
}

// =============================================================================
// coro::detail::ws — server_protocol_cb
// =============================================================================

namespace detail::ws {

namespace {

ListenerState& listener_of(lws* wsi) {
    return *static_cast<ListenerState*>(lws_context_user(lws_get_context(wsi)));
}

bool listener_closed(ListenerState& listener) {
    std::lock_guard lk(listener.accept_mutex);
    return listener.closed;
}

} // namespace

int server_protocol_cb(lws* wsi, lws_callback_reasons reason,
                       void* user, void* in, std::size_t len) {
    // `user` points to sizeof(void*) bytes of lws-managed per-session storage holding
    // the connection's ConnectionState*, or null before ESTABLISHED and after CLOSED.
    // Null for the context-level callbacks, which have no session.
    auto** slot = static_cast<void**>(user);
    auto* state = slot ? static_cast<ConnectionState*>(*slot) : nullptr;

    switch (reason) {

    // Before the 101 response. Return -1 to reject, 0 to accept.
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
        auto& listener = listener_of(wsi);
        if (listener_closed(listener)) return -1;

        if (listener.process_request) {
            coro::WsUpgradeRequest req(wsi);
            if (listener.process_request(req).has_value()) return -1;
        }

        if (listener.select_subprotocol) {
            coro::WsUpgradeRequest req(wsi);
            auto offered = req.offered_subprotocols();
            std::vector<std::string_view> views(offered.begin(), offered.end());
            auto selected = listener.select_subprotocol(std::span<const std::string_view>(views));
            if (selected.empty() && !offered.empty()) return -1;
        }
        break;
    }

    // A client completed the handshake: queue it for accept().
    case LWS_CALLBACK_ESTABLISHED: {
        if (!slot) return -1;
        auto& listener = listener_of(wsi);
        auto conn = std::make_shared<ConnectionState>();
        conn->wsi              = wsi;
        conn->frame_mode       = listener.frame_mode;
        conn->max_message_size = listener.max_message_size;
        Weak<Waker> waker;
        {
            std::lock_guard lk(listener.accept_mutex);
            // The WsListener was dropped after this connection passed the filter.
            // Returning -1 closes it; the slot is still null, so CLOSED ignores it.
            if (listener.closed) return -1;
            conn->self = conn;
            *slot = conn.get();
            listener.pending.push_back(conn);
            waker = listener.accept_waker;
        }
        wake(waker);
        break;
    }

    case LWS_CALLBACK_RECEIVE:
        if (!state) break;
        // lws_is_final_fragment / lws_frame_is_binary are only valid during this callback.
        on_receive(*state, std::span(static_cast<const std::byte*>(in), len),
                   lws_frame_is_binary(wsi) == 0, lws_is_final_fragment(wsi));
        break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (!state) break;
        return on_writeable(*state, wsi);

    // WSI_DESTROY covers a wsi that goes without CLOSED; for one that had CLOSED the
    // slot is already null.
    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_WSI_DESTROY:
        if (!state) break;
        *slot = nullptr;
        close_connection(*state, ECONNABORTED);
        break;

    default:
        break;
    }
    return 0;
}

} // namespace detail::ws

namespace {

// A fresh state per bind attempt: lws keeps pointers into it (the protocols array,
// the context user data), so it belongs to that attempt's context alone.
std::shared_ptr<detail::ws::ListenerState> make_listener_state(const WsListener::Options& options) {
    auto state = std::make_shared<detail::ws::ListenerState>();
    state->frame_mode         = options.frame_mode;
    state->max_frame_size     = options.max_frame_size;
    state->max_message_size   = options.max_message_size;
    state->process_request    = options.process_request;
    state->select_subprotocol = options.select_subprotocol;
    for (const auto& p : options.subprotocols) {
        if (!state->subprotocol_str.empty()) state->subprotocol_str += ',';
        state->subprotocol_str += p;
    }
    const char* name = state->subprotocol_str.empty() ? "coro-ws" : state->subprotocol_str.c_str();
    state->protocols[0] = {name, detail::ws::server_protocol_cb, sizeof(void*),
                           static_cast<unsigned int>(state->max_frame_size), 0, nullptr, 0};
    state->protocols[1] = {nullptr, nullptr, 0, 0, 0, nullptr, 0};
    return state;
}

// lws reports a failed bind only as a null context: it neither returns nor keeps the
// errno. So bind the address again ourselves, with the same SO_REUSEADDR, to learn
// why. Race (benign): the port's state can change in between; the probe only picks
// the error code, and a probe that succeeds reports EIO (lws failed for some reason
// other than the bind).
int bind_errno(const SocketAddress& local) {
    try {
        detail::sys::close_socket(detail::sys::tcp_listen(local, 1));
    } catch (const std::system_error& e) {
        return e.code().value();
    }
    return EIO;
}

} // namespace

// =============================================================================
// WsListener
// =============================================================================

WsListener::WsListener(std::shared_ptr<detail::ws::ListenerState> state,
                       std::shared_ptr<detail::ws::LwsService>    service)
    : m_state(std::move(state))
    , m_service(std::move(service)) {}

WsListener::WsListener(WsListener&&) noexcept = default;

WsListener& WsListener::operator=(WsListener&& other) noexcept {
    if (this != &other) {
        close();
        m_state   = std::move(other.m_state);
        m_service = std::move(other.m_service);
    }
    return *this;
}

WsListener::~WsListener() { close(); }

void WsListener::close() noexcept {
    if (!m_state) return;
    std::deque<std::shared_ptr<detail::ws::ConnectionState>> pending;
    detail::Weak<detail::Waker> waker;
    {
        std::lock_guard lk(m_state->accept_mutex);
        // From here the callbacks reject new connections (FILTER and ESTABLISHED
        // check this under the same mutex), so nothing more reaches `pending`.
        m_state->closed = true;
        pending.swap(m_state->pending);
        waker = m_state->accept_waker;
    }
    // An AcceptFuture from this listener may still be pending in another task.
    detail::ws::wake(waker);
    for (auto& conn : pending) detail::ws::post_close(*m_service, std::move(conn));
    m_state.reset();
    // If no accepted stream holds the service, this destroys the context (closing the
    // listening socket) and joins its thread, after the closes above have run.
    m_service.reset();
}

Coro<WsListener> WsListener::bind(std::string host, uint16_t port) {
    return bind(std::move(host), port, Options{});
}

Coro<WsListener> WsListener::bind(std::string host, uint16_t port, Options options) {
    // nullopt: every interface (lws's iface = nullptr).
    std::vector<std::optional<SocketAddress>> candidates;
    if (host.empty()) {
        candidates.emplace_back(std::nullopt);
    } else {
        for (const SocketAddress& addr : co_await lookup_host(host, port))
            candidates.emplace_back(addr);
    }

    std::exception_ptr last_error;
    for (const auto& candidate : candidates) {
#if !defined(LWS_WITH_IPV6)
        if (candidate && std::holds_alternative<Ipv6Address>(candidate->address)) {
            last_error = std::make_exception_ptr(std::system_error(
                EAFNOSUPPORT, std::system_category(),
                "WsListener::bind: libwebsockets was built without IPv6"));
            continue;
        }
#endif
        auto state = make_listener_state(options);
        const std::string iface = candidate ? detail::ws::numeric_host(*candidate) : std::string();

        lws_context_creation_info info{};
        info.port      = port;
        info.iface     = candidate ? iface.c_str() : nullptr;
        info.protocols = state->protocols;
        info.user      = state.get();
        // Without this, an address lws can't bind yet is deferred instead of failing.
        info.options   = LWS_SERVER_OPTION_FAIL_UPON_UNABLE_TO_BIND;

        // Synchronous: the socket is bound and listening when this returns.
        std::string lws_errors;
        if (auto service = detail::ws::LwsService::create(info, state, &lws_errors))
            co_return WsListener(std::move(state), std::move(service));

        const SocketAddress local = candidate ? *candidate : SocketAddress{Ipv4Address{}, port};
        last_error = std::make_exception_ptr(std::system_error(
            bind_errno(local), std::system_category(), "WsListener::bind (" + lws_errors + ")"));
    }
    std::rethrow_exception(last_error);   // non-null: there is always a candidate
}

WsListener::AcceptFuture WsListener::accept() {
    if (!m_state) throw std::logic_error("WsListener::accept: moved-from WsListener");
    return AcceptFuture(m_state, m_service);
}

// =============================================================================
// AcceptFuture
// =============================================================================

WsListener::AcceptFuture::AcceptFuture(std::shared_ptr<detail::ws::ListenerState> state,
                                       std::shared_ptr<detail::ws::LwsService>    service)
    : m_state(std::move(state))
    , m_service(std::move(service)) {}

PollResult<WsStream> WsListener::AcceptFuture::poll(detail::Context& ctx) {
    std::lock_guard lk(m_state->accept_mutex);

    if (m_state->closed)
        throw std::runtime_error("WsListener::accept: listener is closed");

    if (!m_state->pending.empty()) {
        auto conn = std::move(m_state->pending.front());
        m_state->pending.pop_front();
        // WsStream's private constructor: WsStream befriends WsListener, and
        // AcceptFuture, as its nested class, shares that access.
        return WsStream(std::move(conn), m_service);
    }

    m_state->accept_waker = ctx.get_weak_waker();
    return PollPending;
}

} // namespace coro
