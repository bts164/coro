// Stub implementations of TcpStream and TcpListener for executor-only tests.
//
// Provides the minimum linkage needed to compile test_tcp_stream without a
// real lwIP installation. All methods throw if actually called at runtime —
// the test suite only exercises compile-time properties and executor behaviour,
// not real TCP connections.

#include <coro/io/tcp_stream.h>
#include <coro/io/tcp_listener.h>
#include <stdexcept>

// Forward-declare the context types so the constructors can accept nullptr.
// The types are incomplete here — that is intentional and safe because
// shared_ptr<T>(nullptr) never dereferences T.
namespace coro::detail {
    struct LwipTcpCtx;
    struct LwipListenCtx;
}

namespace coro {

// ---------------------------------------------------------------------------
// TcpStream stubs
// ---------------------------------------------------------------------------

TcpStream::TcpStream(std::shared_ptr<detail::LwipTcpCtx> impl)
    : m_impl(std::move(impl)) {}
TcpStream::TcpStream(TcpStream&&) noexcept = default;
TcpStream& TcpStream::operator=(TcpStream&&) noexcept = default;
TcpStream::~TcpStream() = default; // no PCB to abort

Coro<TcpStream> TcpStream::connect(std::string, uint16_t) {
    throw std::runtime_error("TcpStream::connect: not available in stub build");
    co_return TcpStream(std::shared_ptr<detail::LwipTcpCtx>{});
}

Coro<std::size_t> TcpStream::read_impl(std::byte*, std::size_t) {
    throw std::runtime_error("TcpStream::read_impl: not available in stub build");
    co_return std::size_t{0};
}

Coro<void> TcpStream::write_impl(const std::byte*, std::size_t) {
    throw std::runtime_error("TcpStream::write_impl: not available in stub build");
    co_return;
}

// ---------------------------------------------------------------------------
// TcpListener stubs
// ---------------------------------------------------------------------------

TcpListener::TcpListener(std::shared_ptr<detail::LwipListenCtx> impl)
    : m_impl(std::move(impl)) {}
TcpListener::TcpListener(TcpListener&&) noexcept = default;
TcpListener& TcpListener::operator=(TcpListener&&) noexcept = default;
TcpListener::~TcpListener() = default;

Coro<TcpListener> TcpListener::bind(std::string, uint16_t) {
    throw std::runtime_error("TcpListener::bind: not available in stub build");
    co_return TcpListener(std::shared_ptr<detail::LwipListenCtx>{});
}

Coro<TcpStream> TcpListener::accept() {
    throw std::runtime_error("TcpListener::accept: not available in stub build");
    co_return TcpStream(std::shared_ptr<detail::LwipTcpCtx>{});
}

} // namespace coro
