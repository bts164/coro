#pragma once

// Desktop only: the read and write futures shared by every byte-stream type on the
// IoDriver (TcpStream, Pipe). Each type supplies an `Io` policy naming its
// non-blocking syscalls and its error-message prefixes:
//
//   struct Io {
//       static sys::IoResult try_read (sys::RawFd, std::byte*, std::size_t) noexcept;
//       static sys::IoResult try_write(sys::RawFd, const std::byte*, std::size_t) noexcept;
//       static constexpr const char* read_name, read_exact_name, write_name;
//   };
//
// Every future here is a leaf future (no cancel()): while pending it holds only a weak
// waker in the fd's IoRegistration, so dropping it is always memory-safe. What a drop
// does to the byte stream is documented on each stream type. See
// doc/design/tcp_stream.md, "Byte-stream futures".

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/socket_state.h>
#include <coro/detail/sys/socket.h>
#include <coro/io/byte_buffer.h>

#include <cstddef>
#include <memory>
#include <ranges>
#include <utility>

namespace coro::detail {

/**
 * @brief Read future for a byte stream (`Exact = false`: `read()`; `Exact = true`:
 * `read_exact()`). Yields `{bytes_read, buf}`.
 *
 * Hand-written rather than a `Coro` so a read of already-buffered data costs no
 * coroutine frame: the first `poll()`, which `FutureAwaitable::await_ready()` runs
 * eagerly, reads and returns `PollReady`. On EAGAIN it waits for read readiness.
 *
 * `read()` returns after the first non-empty chunk, or 0 at EOF. `read_exact()` keeps
 * reading into the rest of the buffer until it is full or EOF; how much it has read so
 * far lives in the future, so a dropped `read_exact()` loses those bytes.
 *
 * Race note: overlapping reads on one stream aren't supported; the second would
 * replace the first's waker, leaving the first unwoken, and the two would split the
 * byte stream between them.
 */
template<class Io, ByteBuffer Buf, bool Exact>
class FdReadFuture {
public:
    using OutputType = std::pair<std::size_t, Buf>;

    FdReadFuture(std::shared_ptr<SocketState> state, Buf buf)
        : m_state(std::move(state)), m_buf(std::move(buf)) {}

    PollResult<OutputType> poll(Context& ctx) {
        auto* data = reinterpret_cast<std::byte*>(std::ranges::data(m_buf));
        const std::size_t size = std::ranges::size(m_buf);
        while (m_filled < size) {
            auto result = m_state->reg.poll_io(IoDirection::Read, ctx, [&] {
                return Io::try_read(m_state->fd, data + m_filled, size - m_filled);
            });
            if (!result) return PollPending;
            if (!*result)
                return PollError(socket_error(result->error(),
                                              Exact ? Io::read_exact_name : Io::read_name));
            if (**result == 0) break;   // EOF
            m_filled += **result;
            if constexpr (!Exact) break;
        }
        return OutputType{m_filled, std::move(m_buf)};
    }

private:
    std::shared_ptr<SocketState> m_state;
    Buf                          m_buf;
    std::size_t                  m_filled = 0;
};

/**
 * @brief Write future for a byte stream. Yields `buf` once every byte has been
 * written.
 *
 * Hand-written for the same reason as `FdReadFuture`: when the kernel buffer has room,
 * the first `poll()` writes everything with no coroutine frame. A partial write keeps
 * its progress in the future and waits for write readiness before sending the rest.
 *
 * Race note: overlapping writes on one stream aren't supported; the second would
 * replace the first's waker, and their bytes could interleave.
 */
template<class Io, ByteBuffer Buf>
class FdWriteFuture {
public:
    using OutputType = Buf;

    FdWriteFuture(std::shared_ptr<SocketState> state, Buf buf)
        : m_state(std::move(state)), m_buf(std::move(buf)) {}

    PollResult<Buf> poll(Context& ctx) {
        const auto* data = reinterpret_cast<const std::byte*>(std::ranges::data(m_buf));
        const std::size_t size = std::ranges::size(m_buf);
        while (m_written < size) {
            auto result = m_state->reg.poll_io(IoDirection::Write, ctx, [&] {
                return Io::try_write(m_state->fd, data + m_written, size - m_written);
            });
            if (!result) return PollPending;
            if (!*result) return PollError(socket_error(result->error(), Io::write_name));
            m_written += **result;
        }
        return std::move(m_buf);
    }

private:
    std::shared_ptr<SocketState> m_state;
    Buf                          m_buf;
    std::size_t                  m_written = 0;
};

} // namespace coro::detail
