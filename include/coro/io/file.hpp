#pragma once

// Template implementations for File::read/read_exact/read_at/read_at_exact
// and the write equivalents. Included at the bottom of file.h.

#include <coro/io/file.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <ranges>
#include <system_error>
#include <utility>

namespace coro {

// ---------------------------------------------------------------------------
// read_at — single uv_fs_read call
// ---------------------------------------------------------------------------

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::read_at(Buf buf, int64_t offset) {
    return with_context(*m_exec,
        [](uv_file fd, Buf buf, int64_t offset) -> Coro<std::pair<std::size_t, Buf>> {
            UvCallbackResult<ssize_t> result;
            uv_fs_t req;
            // buf lives on this coroutine frame; the frame is kept alive by co_await,
            // so bdesc.base remains valid for the entire libuv operation.
            uv_buf_t bdesc = uv_buf_init(
                reinterpret_cast<char*>(std::ranges::data(buf)),
                static_cast<unsigned>(std::ranges::size(buf)));
            req.data = &result;

            int r = uv_fs_read(current_uv_executor().loop(), &req,
                               fd, &bdesc, 1, offset,
                [](uv_fs_t* r) {
                    static_cast<decltype(result)*>(r->data)->complete(r->result);
                    uv_fs_req_cleanup(r);
                });
            if (r < 0)
                throw std::system_error(
                    std::error_code(-r, std::system_category()), "uv_fs_read");

            auto [nbytes] = co_await wait(result);
            if (nbytes < 0)
                throw std::system_error(
                    std::error_code(static_cast<int>(-nbytes), std::system_category()),
                    "uv_fs_read");
            co_return std::pair<std::size_t, Buf>{
                static_cast<std::size_t>(nbytes), std::move(buf)};
        }(m_fd, std::move(buf), offset)
    );
}

// ---------------------------------------------------------------------------
// read_at_exact — loops until buf is full or EOF
// ---------------------------------------------------------------------------

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::read_at_exact(Buf buf, int64_t offset) {
    return with_context(*m_exec,
        [](uv_file fd, Buf buf, int64_t offset) -> Coro<std::pair<std::size_t, Buf>> {
            const std::size_t size = std::ranges::size(buf);
            std::size_t n = 0;
            while (n < size) {
                UvCallbackResult<ssize_t> result;
                uv_fs_t req;
                uv_buf_t bdesc = uv_buf_init(
                    reinterpret_cast<char*>(std::ranges::data(buf)) + n,
                    static_cast<unsigned>(size - n));
                req.data = &result;

                // Advance the positional offset on each iteration; -1 means
                // "use current position" and the kernel advances it automatically.
                int64_t cur = offset < 0 ? offset : offset + static_cast<int64_t>(n);
                int r = uv_fs_read(current_uv_executor().loop(), &req,
                                   fd, &bdesc, 1, cur,
                    [](uv_fs_t* r) {
                        static_cast<decltype(result)*>(r->data)->complete(r->result);
                        uv_fs_req_cleanup(r);
                    });
                if (r < 0)
                    throw std::system_error(
                        std::error_code(-r, std::system_category()), "uv_fs_read");

                auto [nbytes] = co_await wait(result);
                if (nbytes < 0)
                    throw std::system_error(
                        std::error_code(static_cast<int>(-nbytes), std::system_category()),
                        "uv_fs_read");
                if (nbytes == 0) break; // EOF
                n += static_cast<std::size_t>(nbytes);
            }
            co_return std::pair<std::size_t, Buf>{n, std::move(buf)};
        }(m_fd, std::move(buf), offset)
    );
}

// ---------------------------------------------------------------------------
// read / read_exact — delegate to the _at variants with offset=-1
// ---------------------------------------------------------------------------

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::read(Buf buf) {
    return read_at(std::move(buf), -1);
}

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::read_exact(Buf buf) {
    return read_at_exact(std::move(buf), -1);
}

// ---------------------------------------------------------------------------
// write_at — single uv_fs_write call
// ---------------------------------------------------------------------------

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::write_at(Buf buf, int64_t offset) {
    return with_context(*m_exec,
        [](uv_file fd, Buf buf, int64_t offset) -> Coro<std::pair<std::size_t, Buf>> {
            UvCallbackResult<ssize_t> result;
            uv_fs_t req;
            // uv_buf_t.base is char* but uv_fs_write does not modify the buffer.
            uv_buf_t bdesc = uv_buf_init(
                reinterpret_cast<char*>(std::ranges::data(buf)),
                static_cast<unsigned>(std::ranges::size(buf)));
            req.data = &result;

            int r = uv_fs_write(current_uv_executor().loop(), &req,
                                fd, &bdesc, 1, offset,
                [](uv_fs_t* r) {
                    static_cast<decltype(result)*>(r->data)->complete(r->result);
                    uv_fs_req_cleanup(r);
                });
            if (r < 0)
                throw std::system_error(
                    std::error_code(-r, std::system_category()), "uv_fs_write");

            auto [nbytes] = co_await wait(result);
            if (nbytes < 0)
                throw std::system_error(
                    std::error_code(static_cast<int>(-nbytes), std::system_category()),
                    "uv_fs_write");
            co_return std::pair<std::size_t, Buf>{
                static_cast<std::size_t>(nbytes), std::move(buf)};
        }(m_fd, std::move(buf), offset)
    );
}

// ---------------------------------------------------------------------------
// write_at_exact — loops until all bytes are written
// ---------------------------------------------------------------------------

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::write_at_exact(Buf buf, int64_t offset) {
    return with_context(*m_exec,
        [](uv_file fd, Buf buf, int64_t offset) -> Coro<std::pair<std::size_t, Buf>> {
            const std::size_t size = std::ranges::size(buf);
            std::size_t n = 0;
            while (n < size) {
                UvCallbackResult<ssize_t> result;
                uv_fs_t req;
                uv_buf_t bdesc = uv_buf_init(
                    reinterpret_cast<char*>(std::ranges::data(buf)) + n,
                    static_cast<unsigned>(size - n));
                req.data = &result;

                int64_t cur = offset < 0 ? offset : offset + static_cast<int64_t>(n);
                int r = uv_fs_write(current_uv_executor().loop(), &req,
                                    fd, &bdesc, 1, cur,
                    [](uv_fs_t* r) {
                        static_cast<decltype(result)*>(r->data)->complete(r->result);
                        uv_fs_req_cleanup(r);
                    });
                if (r < 0)
                    throw std::system_error(
                        std::error_code(-r, std::system_category()), "uv_fs_write");

                auto [nbytes] = co_await wait(result);
                if (nbytes < 0)
                    throw std::system_error(
                        std::error_code(static_cast<int>(-nbytes), std::system_category()),
                        "uv_fs_write");
                n += static_cast<std::size_t>(nbytes);
            }
            co_return std::pair<std::size_t, Buf>{n, std::move(buf)};
        }(m_fd, std::move(buf), offset)
    );
}

// ---------------------------------------------------------------------------
// write / write_exact — delegate to the _at variants with offset=-1
// ---------------------------------------------------------------------------

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::write(Buf buf) {
    return write_at(std::move(buf), -1);
}

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::write_exact(Buf buf) {
    return write_at_exact(std::move(buf), -1);
}

} // namespace coro
