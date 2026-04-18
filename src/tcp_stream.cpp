#include <coro/io/tcp_stream.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <system_error>

namespace coro {

namespace {
[[noreturn]] void throw_uv_error(int status, const char* what) {
    throw std::system_error(
        std::error_code(-status, std::system_category()), what);
}
} // namespace

TcpStream::TcpStream(std::shared_ptr<Handle> handle, SingleThreadedUvExecutor* uv_exec)
    : m_handle(std::move(handle)), m_uv_exec(uv_exec) {}

TcpStream::TcpStream(TcpStream&&) noexcept = default;
TcpStream& TcpStream::operator=(TcpStream&&) noexcept = default;

TcpStream::~TcpStream() {
    if (!m_handle) return;
    with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle) -> Coro<void> {
            UvCallbackResult<int> result;
            handle->handle.data = &result;
            uv_close(reinterpret_cast<uv_handle_t*>(&handle->handle),
                [](uv_handle_t* h) {
                    static_cast<UvCallbackResult<int>*>(h->data)->complete(0);
                });
            auto [ignored] = co_await wait(result);
            (void)ignored;
        }(std::move(m_handle))
    ).detach();
}

// ---------------------------------------------------------------------------
// connect
// ---------------------------------------------------------------------------

TcpStream::ConnectFuture TcpStream::connect(std::string host, uint16_t port) {
    auto& exec = current_uv_executor();
    return with_context(exec,
        [](SingleThreadedUvExecutor& exec,
           std::string host, uint16_t port) -> Coro<TcpStream> {

            auto handle = std::make_shared<Handle>();
            uv_tcp_init(exec.loop(), &handle->handle);

            struct sockaddr_in addr;
            if (int r = uv_ip4_addr(host.c_str(), port, &addr); r != 0)
                throw_uv_error(r, "TcpStream::connect");

            UvCallbackResult<int> result;
            uv_connect_t req;
            req.data = &result;

            int r = uv_tcp_connect(&req, &handle->handle,
                reinterpret_cast<const struct sockaddr*>(&addr),
                [](uv_connect_t* r, int status) {
                    static_cast<UvCallbackResult<int>*>(r->data)->complete(status);
                });
            if (r != 0)
                throw_uv_error(r, "TcpStream::connect");

            auto [status] = co_await wait(result);
            if (status != 0)
                throw_uv_error(status, "TcpStream::connect");

            co_return TcpStream(std::move(handle), &exec);
        }(exec, std::move(host), port)
    );
}


} // namespace coro
