#pragma once

// Template implementations for TcpStream::read<Buf> and TcpStream::write<Buf>.
// Included at the bottom of tcp_stream.h — not meant to be included directly.

#include <coro/io/tcp_stream.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <span>
#include <system_error>
#include <utility>

namespace coro {

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> TcpStream::read(Buf buf) {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle, Buf buf) -> Coro<std::pair<std::size_t, Buf>> {
            // ReadCtx lives on this coroutine frame. The frame is heap-allocated
            // and kept alive by co_await, so the span and result pointer are
            // stable for the entire duration of the libuv read.
            struct ReadCtx {
                std::span<std::byte>      view;
                UvCallbackResult<ssize_t> result;
            };
            ReadCtx ctx{std::as_writable_bytes(std::span(buf))};
            handle->handle.data = &ctx;

            int r = uv_read_start(
                reinterpret_cast<uv_stream_t*>(&handle->handle),
                [](uv_handle_t* h, std::size_t, uv_buf_t* out) {
                    auto& c = *static_cast<ReadCtx*>(h->data);
                    out->base = reinterpret_cast<char*>(c.view.data());
                    out->len  = static_cast<unsigned>(c.view.size());
                },
                [](uv_stream_t* s, ssize_t nread, const uv_buf_t*) {
                    if (nread == UV_EAGAIN) return;
                    uv_read_stop(s);
                    auto& c = *static_cast<ReadCtx*>(
                        reinterpret_cast<uv_handle_t*>(s)->data);
                    c.result.complete(nread == UV_EOF ? ssize_t{0} : nread);
                });
            if (r < 0)
                throw std::system_error(
                    std::error_code(-r, std::system_category()), "TcpStream::read");

            auto [nread] = co_await wait(ctx.result);
            handle->handle.data = nullptr;

            if (nread < 0)
                throw std::system_error(
                    std::error_code(static_cast<int>(-nread), std::system_category()),
                    "TcpStream::read");
            co_return std::pair<std::size_t, Buf>{static_cast<std::size_t>(nread), std::move(buf)};
        }(m_handle, std::move(buf))
    );
}

template <ByteBuffer Buf>
JoinHandle<Buf> TcpStream::write(Buf buf) {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<Handle> handle, Buf buf) -> Coro<Buf> {
            // view points into buf on this coroutine frame; stable across the
            // co_await because the frame outlives the uv_write callback.
            auto view = std::as_bytes(std::span(buf));

            UvCallbackResult<int> result;
            uv_write_t req;
            // uv_buf_t.base is char* but uv_write does not modify the buffer;
            // const_cast is safe here.
            uv_buf_t bdesc = uv_buf_init(
                const_cast<char*>(reinterpret_cast<const char*>(view.data())),
                static_cast<unsigned>(view.size()));
            req.data = &result;

            int r = uv_write(&req,
                reinterpret_cast<uv_stream_t*>(&handle->handle),
                &bdesc, 1,
                [](uv_write_t* r, int status) {
                    static_cast<UvCallbackResult<int>*>(r->data)->complete(status);
                });
            if (r < 0)
                throw std::system_error(
                    std::error_code(-r, std::system_category()), "TcpStream::write");

            auto [status] = co_await wait(result);
            if (status != 0)
                throw std::system_error(
                    std::error_code(-status, std::system_category()), "TcpStream::write");
            co_return std::move(buf);
        }(m_handle, std::move(buf))
    );
}

} // namespace coro
