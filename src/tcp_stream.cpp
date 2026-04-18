#include <coro/io/tcp_stream.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <stdexcept>
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

// ---------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------

TcpStream::ReadFuture TcpStream::read(std::span<std::byte> buf) {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle, std::span<std::byte> buf) -> Coro<std::size_t> {
            // Both alloc_cb and read_cb recover state via handle.data.
            // ReadCtx lives on this coroutine frame — safe because co_await keeps
            // the frame alive until read_cb fires and calls result.complete().
            struct ReadCtx {
                std::span<std::byte>      buf;
                UvCallbackResult<ssize_t> result;
            };
            ReadCtx ctx{buf};
            handle->handle.data = &ctx;

            int r = uv_read_start(
                reinterpret_cast<uv_stream_t*>(&handle->handle),
                [](uv_handle_t* h, std::size_t, uv_buf_t* out) {
                    auto& c = *static_cast<ReadCtx*>(h->data);
                    out->base = reinterpret_cast<char*>(c.buf.data());
                    out->len  = static_cast<unsigned>(c.buf.size());
                },
                [](uv_stream_t* s, ssize_t nread, const uv_buf_t*) {
                    if (nread == UV_EAGAIN) return;
                    uv_read_stop(s);
                    auto& c = *static_cast<ReadCtx*>(
                        reinterpret_cast<uv_handle_t*>(s)->data);
                    c.result.complete(nread == UV_EOF ? ssize_t{0} : nread);
                });
            if (r < 0)
                throw_uv_error(r, "TcpStream::read");

            auto [nread] = co_await wait(ctx.result);
            handle->handle.data = nullptr;

            if (nread < 0)
                throw_uv_error(static_cast<int>(nread), "TcpStream::read");
            co_return static_cast<std::size_t>(nread);
        }(m_handle, buf)
    );
}

// ---------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------

TcpStream::WriteFuture TcpStream::write(std::span<const std::byte> data) {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle,
           std::span<const std::byte> data) -> Coro<void> {

            UvCallbackResult<int> result;
            uv_write_t req;
            uv_buf_t bdesc = uv_buf_init(
                const_cast<char*>(reinterpret_cast<const char*>(data.data())),
                static_cast<unsigned>(data.size()));
            req.data = &result;

            int r = uv_write(&req,
                reinterpret_cast<uv_stream_t*>(&handle->handle),
                &bdesc, 1,
                [](uv_write_t* r, int status) {
                    static_cast<UvCallbackResult<int>*>(r->data)->complete(status);
                });
            if (r < 0)
                throw_uv_error(r, "TcpStream::write");

            auto [status] = co_await wait(result);
            if (status != 0)
                throw_uv_error(status, "TcpStream::write");
        }(m_handle, data)
    );
}

} // namespace coro
