#pragma once

// Desktop only: named pipes (FIFOs) on the IoDriver. See doc/design/pipe_streaming.md.

#include <coro/coro.h>
#include <coro/detail/socket_state.h>
#include <coro/detail/stream_io.h>
#include <coro/detail/sys/pipe.h>
#include <coro/io/byte_buffer.h>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace coro {

// ---------------------------------------------------------------------------
// PipeMode — flags for Pipe::open()
// ---------------------------------------------------------------------------

enum class PipeMode : unsigned {
    Read      = 0x01,  // waits until a writer has written or come and gone
    Write     = 0x02,  // waits until a reader has the FIFO open
    ReadWrite = 0x03,  // never waits, and never reads EOF
};

namespace detail {
/// Pipe's syscalls for the shared byte-stream futures (detail/stream_io.h).
struct PipeIo {
    static sys::IoResult try_read(sys::RawFd fd, std::byte* data, std::size_t size) noexcept {
        return sys::pipe_try_read(fd, data, size);
    }
    static sys::IoResult try_write(sys::RawFd fd, const std::byte* data,
                                   std::size_t size) noexcept {
        return sys::pipe_try_write(fd, data, size);
    }
    static constexpr const char* read_name       = "Pipe::read";
    static constexpr const char* read_exact_name = "Pipe::read_exact";
    static constexpr const char* write_name      = "Pipe::write";
};
} // namespace detail

/// Future returned by `Pipe::read()` (`Exact = false`) and `read_exact()`
/// (`Exact = true`). Yields `{bytes_read, buf}`.
template<ByteBuffer Buf, bool Exact>
using PipeReadFuture = detail::FdReadFuture<detail::PipeIo, Buf, Exact>;

/// Future returned by `Pipe::write()`. Yields `buf` once every byte is written.
template<ByteBuffer Buf>
using PipeWriteFuture = detail::FdWriteFuture<detail::PipeIo, Buf>;

// ---------------------------------------------------------------------------
// Pipe
// ---------------------------------------------------------------------------

/**
 * @brief Async named-pipe (FIFO) handle. Move-only; obtain via
 * `co_await Pipe::open(path, mode)` after creating the FIFO with
 * `co_await Pipe::create(path)`.
 *
 * Same shape as `TcpStream`: every operation is a non-blocking syscall on the calling
 * thread, and one that would block waits for readiness from the Runtime's IoDriver.
 * Requires an executor that turns the IoDriver (`Runtime(n)` for any n); `open()`
 * throws `std::logic_error` otherwise.
 *
 * **Concurrency:** only one read (`read()`/`read_exact()`) and only one `write()` may
 * be in flight at a time (a read and a write at once only makes sense in
 * `PipeMode::ReadWrite`).
 *
 * **Cancellation:** dropping a pending `open()` or `read()` is always safe and loses
 * nothing. Dropping a pending `read_exact()` loses the bytes it already read, and
 * dropping a pending `write()` may leave part of the buffer written: after either,
 * the byte stream is out of step.
 *
 * **Closing:** the fd is closed synchronously when the last owner drops it: the Pipe
 * or a still-pending future.
 *
 * @warning A write after the reader has closed raises SIGPIPE, which kills the process
 *          unless it is ignored or handled (`signal(SIGPIPE, SIG_IGN)`); the write then
 *          throws EPIPE. Unlike `TcpStream`, a FIFO write has no per-call way to
 *          suppress it.
 */
class Pipe {
public:
    Pipe(Pipe&&) noexcept;
    Pipe& operator=(Pipe&&) noexcept;
    Pipe(const Pipe&)            = delete;
    Pipe& operator=(const Pipe&) = delete;

    /// Releases this handle. The fd is deregistered and closed synchronously once no
    /// in-flight future still uses it.
    ~Pipe();

    /**
     * @brief Opens an existing FIFO at `path`.
     *
     * - `PipeMode::Write` waits until a reader has the FIFO open. The kernel gives no
     *   event for a reader arriving, so this retries the open on a timer (1 ms, backing
     *   off to 50 ms).
     * - `PipeMode::Read` waits until a writer has written data, or has opened and closed
     *   again (the first read then returns 0). Before any writer arrives a FIFO read
     *   would return 0 as if at EOF; waiting here keeps EOF meaning "the writers are
     *   gone". A writer that opens and writes nothing yet keeps this waiting.
     * - `PipeMode::ReadWrite` never waits.
     *
     * @throws std::system_error (at co_await) on failure, e.g. ENOENT.
     * @throws std::logic_error if the current Runtime's executor doesn't turn the
     *         IoDriver.
     */
    [[nodiscard]] static Coro<Pipe> open(std::string path, PipeMode mode);

    /**
     * @brief Creates a FIFO at `path` via mkfifo(3). An existing file at `path` is
     * silently accepted, whatever its type.
     * @param permission Unix permission bits (default 0666, subject to umask).
     * @throws std::system_error (at co_await) on failure.
     */
    [[nodiscard]] static Coro<void> create(std::string path, int permission = 0666);

    /**
     * @brief Reads up to `buf.size()` bytes into `buf` and returns `{bytes_read, buf}`;
     * 0 bytes once every writer has closed.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] PipeReadFuture<Buf, false> read(Buf buf);

    /**
     * @brief Reads exactly `buf.size()` bytes. Returns `{bytes_read, buf}`;
     * `bytes_read < buf.size()` indicates EOF before the buffer was filled.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template<ByteBuffer Buf>
    [[nodiscard]] PipeReadFuture<Buf, true> read_exact(Buf buf);

    /**
     * @brief Writes all of `buf` and returns `buf` once the kernel has accepted every
     * byte. Writes of at most `PIPE_BUF` bytes (4096 on Linux) are atomic with respect
     * to other writers of the same FIFO; larger ones may interleave.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     * @throws std::system_error (at co_await) on failure, e.g. EPIPE (see the SIGPIPE
     *         warning above).
     */
    template<ByteBuffer Buf>
    [[nodiscard]] PipeWriteFuture<Buf> write(Buf buf);

private:
    using State = detail::SocketState;

    explicit Pipe(std::shared_ptr<State> state);

    std::shared_ptr<State> m_state;
};

} // namespace coro

#include <coro/io/pipe.hpp>
