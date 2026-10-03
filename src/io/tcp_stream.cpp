// Desktop TcpStream on the IoDriver. Every syscall goes through the backend seam in
// include/coro/detail/sys/tcp.h. See doc/design/tcp_stream.md.

#include <coro/io/tcp_stream.h>
#include <coro/detail/sys/tcp.h>
#include <coro/io/lookup_host.h>
#include <coro/io/socket_address.h>
#include <exception>
#include <system_error>

namespace coro {

namespace {

/// Waits for a connect started by tcp_connect_start() to finish: write readiness,
/// then SO_ERROR. A leaf future, so dropping the connect mid-handshake just drops the
/// state, which closes the socket.
class ConnectFuture {
public:
    using OutputType = void;

    explicit ConnectFuture(std::shared_ptr<detail::SocketState> state)
        : m_state(std::move(state)) {}

    PollResult<void> poll(detail::Context& ctx) {
        auto result = m_state->reg.poll_io(IoDirection::Write, ctx, [this] {
            return detail::sys::tcp_try_finish_connect(m_state->fd);
        });
        if (!result) return PollPending;
        if (!*result) return PollError(detail::socket_error(result->error(), "TcpStream::connect"));
        return PollReady;
    }

private:
    std::shared_ptr<detail::SocketState> m_state;
};

} // namespace

TcpStream::TcpStream(std::shared_ptr<State> state) : m_state(std::move(state)) {}

TcpStream::TcpStream(TcpStream&&) noexcept = default;
TcpStream& TcpStream::operator=(TcpStream&&) noexcept = default;
TcpStream::~TcpStream() = default;

Coro<TcpStream> TcpStream::connect(std::string host, uint16_t port) {
    IoDriver& driver = detail::socket_io_driver("TcpStream::connect");
    // Each address the name has, in the resolver's order: the first that connects
    // wins; if none does, the last one's error is thrown (as tokio does). A numeric
    // host resolves to itself without leaving this thread.
    const std::vector<SocketAddress> peers = co_await lookup_host(host, port);
    std::exception_ptr last_error;
    for (const SocketAddress& peer : peers) {
        try {
            // Registered after connect() starts. A handshake that finishes in between
            // is still seen: readiness starts set, so the first poll tries to finish.
            auto state = std::make_shared<State>(driver, detail::sys::tcp_connect_start(peer));
            co_await ConnectFuture(state);
            co_return TcpStream(std::move(state));
        } catch (const std::system_error&) {
            last_error = std::current_exception();
        }
    }
    std::rethrow_exception(last_error);   // non-null: lookup_host never returns empty
}

} // namespace coro
