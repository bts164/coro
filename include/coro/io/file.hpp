#pragma once

// Template implementations for File's reads and writes. Included at the bottom of
// file.h.

#include <coro/io/file.h>
#include <ranges>
#include <system_error>
#include <utility>

namespace coro {

template <bool Write, bool Exact, ByteBuffer Buf>
File::IoFuture<Buf> File::transfer(Buf buf, int64_t offset, const char* what) {
    // The job owns the buffer and a share of the fd. Neither borrows from the caller,
    // so a dropped future just leaves the job to finish and discard its result.
    return spawn_blocking(
        [state = state(what), buf = std::move(buf), offset, what]() mutable
            -> std::pair<std::size_t, Buf> {
            auto* data = reinterpret_cast<std::byte*>(std::ranges::data(buf));
            const std::size_t size = std::ranges::size(buf);
            std::size_t done = 0;
            while (done < size) {
                const int64_t at = offset < 0 ? -1 : offset + static_cast<int64_t>(done);
                detail::sys::IoResult result;
                if constexpr (Write)
                    result = detail::sys::file_write(state->fd, data + done, size - done, at);
                else
                    result = detail::sys::file_read(state->fd, data + done, size - done, at);
                if (!result)
                    throw std::system_error(result.error(), std::system_category(), what);
                // 0 is EOF for a read. A write of a non-empty range returning 0 doesn't
                // happen on regular files; stopping keeps it from spinning if it does.
                if (*result == 0) break;
                done += *result;
                if constexpr (!Exact) break;
            }
            return {done, std::move(buf)};
        });
}

template <ByteBuffer Buf>
File::IoFuture<Buf> File::read(Buf buf) {
    return transfer<false, false>(std::move(buf), -1, "coro::File::read");
}

template <ByteBuffer Buf>
File::IoFuture<Buf> File::read_exact(Buf buf) {
    return transfer<false, true>(std::move(buf), -1, "coro::File::read_exact");
}

template <ByteBuffer Buf>
File::IoFuture<Buf> File::read_at(Buf buf, int64_t offset) {
    return transfer<false, false>(std::move(buf), offset, "coro::File::read_at");
}

template <ByteBuffer Buf>
File::IoFuture<Buf> File::read_at_exact(Buf buf, int64_t offset) {
    return transfer<false, true>(std::move(buf), offset, "coro::File::read_at_exact");
}

template <ByteBuffer Buf>
File::IoFuture<Buf> File::write(Buf buf) {
    return transfer<true, false>(std::move(buf), -1, "coro::File::write");
}

template <ByteBuffer Buf>
File::IoFuture<Buf> File::write_exact(Buf buf) {
    return transfer<true, true>(std::move(buf), -1, "coro::File::write_exact");
}

template <ByteBuffer Buf>
File::IoFuture<Buf> File::write_at(Buf buf, int64_t offset) {
    return transfer<true, false>(std::move(buf), offset, "coro::File::write_at");
}

template <ByteBuffer Buf>
File::IoFuture<Buf> File::write_at_exact(Buf buf, int64_t offset) {
    return transfer<true, true>(std::move(buf), offset, "coro::File::write_at_exact");
}

} // namespace coro
