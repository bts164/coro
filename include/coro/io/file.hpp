#pragma once

// Template implementations for File::read/read_at/write/write_at.
// Included at the bottom of file.h — not meant to be included directly.

#include <coro/io/file.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <ranges>
#include <system_error>
#include <utility>

namespace coro {

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::read_at(Buf buf, int64_t offset, bool exact) {
    return with_context(*m_exec,
        [](uv_file fd, Buf buf, int64_t offset, bool exact) -> Coro<std::pair<std::size_t, Buf>> {
            std::size_t n = 0;
            do {
                UvCallbackResult<ssize_t> result;
                uv_fs_t req;
                // buf lives on this coroutine frame; the frame is kept alive by co_await,
                // so bdesc.base remains valid for the entire libuv operation.
                uv_buf_t bdesc = uv_buf_init(
                    reinterpret_cast<char*>(std::ranges::data(buf)) + n,
                    static_cast<unsigned>(std::ranges::size(buf) - n));
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
                if (nbytes == 0) break; // EOF
                n += static_cast<std::size_t>(nbytes);
            } while (exact && n < std::ranges::size(buf));

            co_return std::pair<std::size_t, Buf>{n, std::move(buf)};
        }(m_fd, std::move(buf), offset, exact)
    );
}

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::read(Buf buf, bool exact) {
    return read_at(std::move(buf), -1, exact);
}

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::write_at(Buf buf, int64_t offset, bool exact) {
    return with_context(*m_exec,
        [](uv_file fd, Buf buf, int64_t offset, bool exact) -> Coro<std::pair<std::size_t, Buf>> {
            std::size_t n = 0;
            do {
                UvCallbackResult<ssize_t> result;
                uv_fs_t req;
                // uv_buf_t.base is char* but uv_fs_write does not modify the buffer.
                uv_buf_t bdesc = uv_buf_init(
                    reinterpret_cast<char*>(std::ranges::data(buf)) + n,
                    static_cast<unsigned>(std::ranges::size(buf) - n));
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
                n += static_cast<std::size_t>(nbytes);
            } while (exact && n < std::ranges::size(buf));

            co_return std::pair<std::size_t, Buf>{n, std::move(buf)};
        }(m_fd, std::move(buf), offset, exact)
    );
}

template <ByteBuffer Buf>
JoinHandle<std::pair<std::size_t, Buf>> File::write(Buf buf, bool exact) {
    return write_at(std::move(buf), -1, exact);
}

} // namespace coro
