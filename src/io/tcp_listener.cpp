// Desktop TcpListener on the IoDriver. Every syscall goes through the backend seam in
// include/coro/detail/sys/tcp.h. See doc/design/tcp_stream.md, "TcpListener".

#include <coro/io/tcp_listener.h>
#include <coro/detail/sys/tcp.h>
#include <coro/io/lookup_host.h>
#include <coro/io/socket_address.h>
#include <exception>
#include <system_error>

namespace coro {

namespace {
// The common default (SOMAXCONN on older Linux). The kernel caps it at
// net.core.somaxconn.
constexpr int kListenBacklog = 128;
} // namespace

// ---------------------------------------------------------------------------
// TcpAcceptFuture
// ---------------------------------------------------------------------------

PollResult<TcpStream> TcpAcceptFuture::poll(detail::Context& ctx) {
    // Race note: overlapping accept() calls on one listener aren't supported; the
    // second would replace the first's waker, leaving the first unwoken.
    auto result = m_listener->reg.poll_io(IoDirection::Read, ctx, [this] {
        return detail::sys::tcp_try_accept(m_listener->fd);
    });
    if (!result) return PollPending;
    if (!*result) return PollError(detail::socket_error(result->error(), "TcpListener::accept"));
    try {
        // The connection registers with the listener's driver. SocketState closes the
        // fd itself if the registration throws.
        return TcpStream(std::make_shared<detail::SocketState>(*m_listener->driver, **result));
    } catch (...) {
        return PollError(std::current_exception());
    }
}

// ---------------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------------

TcpListener::TcpListener(std::shared_ptr<State> state) : m_state(std::move(state)) {}

TcpListener::TcpListener(TcpListener&&) noexcept = default;
TcpListener& TcpListener::operator=(TcpListener&&) noexcept = default;
TcpListener::~TcpListener() = default;

Coro<TcpListener> TcpListener::bind(std::string host, uint16_t port) {
    IoDriver& driver = detail::socket_io_driver("TcpListener::bind");
    // The first resolved address that binds wins, as for TcpStream::connect.
    const std::vector<SocketAddress> locals = co_await lookup_host(host, port);
    std::exception_ptr last_error;
    for (const SocketAddress& local : locals) {
        try {
            co_return TcpListener(std::make_shared<State>(
                driver, detail::sys::tcp_listen(local, kListenBacklog)));
        } catch (const std::system_error&) {
            last_error = std::current_exception();
        }
    }
    std::rethrow_exception(last_error);   // non-null: lookup_host never returns empty
}

TcpAcceptFuture TcpListener::accept() {
    return TcpAcceptFuture(m_state);
}

} // namespace coro
