#pragma once

// Desktop only: regular files on the Runtime's blocking pool. See
// doc/design/file_io.md.

#include <coro/detail/sys/file.h>
#include <coro/io/byte_buffer.h>
#include <coro/task/spawn_blocking.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace coro {

// ---------------------------------------------------------------------------
// FileMode — user-facing flags for File::open()
// ---------------------------------------------------------------------------

enum class FileMode : unsigned {
    Read      = 0x01,  // O_RDONLY
    Write     = 0x02,  // O_WRONLY
    ReadWrite = 0x03,  // O_RDWR
    Create    = 0x10,  // O_CREAT  — create if not exists
    Truncate  = 0x20,  // O_TRUNC  — truncate to zero length on open
    Append    = 0x40,  // O_APPEND — writes always go to end
};

/// Allow combining FileMode flags with |
constexpr FileMode operator|(FileMode a, FileMode b) {
    return static_cast<FileMode>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

constexpr FileMode operator&(FileMode a, FileMode b) {
    return static_cast<FileMode>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
}

namespace detail {

/// A File's fd, shared with every in-flight operation on it, so the fd is closed only
/// after the last one finishes: never under a read still running on the blocking pool.
struct FileState {
    explicit FileState(sys::RawFd fd_) noexcept : fd(fd_) {}
    /// Closes the fd, on whichever thread drops the last owner.
    ~FileState() { sys::file_close(fd); }

    FileState(const FileState&)            = delete;
    FileState& operator=(const FileState&) = delete;

    sys::RawFd fd;
};

} // namespace detail

// ---------------------------------------------------------------------------
// File
// ---------------------------------------------------------------------------

/**
 * @brief Async file handle. Every operation runs as one job on the Runtime's blocking
 * pool (@ref spawn_blocking), as tokio's `tokio::fs` does.
 *
 * Obtain a `File` via `co_await File::open(path, mode)`. Every method hands its buffer
 * to the job and returns a future yielding the buffer back with the byte count once
 * the syscall has actually finished; there is no write-behind, so a completed
 * `write()` has reached the kernel (not necessarily the disk: see `sync_all()`).
 * The `_exact` variants loop inside one job, so they cost one thread hop, not one per
 * chunk.
 *
 * Operations start when called (they are `BlockingHandle`s, not lazy futures).
 * Dropping one doesn't stop it: the job runs to completion and its result is
 * discarded. For `read()`/`write()` at the file position that means the position
 * still moves.
 *
 * The fd is closed when the `File` and every operation started on it are gone, on
 * whichever thread drops the last one. Closing is synchronous, so reopening the same
 * path right after dropping a `File` is safe.
 *
 * **Concurrency:** `read_at()`/`write_at()` calls may overlap freely. Overlapping
 * `read()`/`write()` calls, which use the shared file position, run in an unspecified
 * order. A `File` must not be moved or assigned while another task is calling it.
 *
 * Needs a Runtime (for its blocking pool) but not one that turns the IoDriver.
 */
class File {
public:
    using OpenFuture = BlockingHandle<File>;
    template <ByteBuffer Buf>
    using IoFuture = BlockingHandle<std::pair<std::size_t, Buf>>;

    File(File&&) noexcept            = default;
    File& operator=(File&&) noexcept = default;
    File(const File&)                = delete;
    File& operator=(const File&)     = delete;

    /// Releases this handle's share of the fd; see the class comment for when it closes.
    ~File() = default;

    /**
     * @brief Opens a file at `path` with the given mode, close-on-exec. `Create`
     * makes it with mode 0644 (subject to umask).
     * @return An `OpenFuture` that resolves to a `File` handle.
     * @throws std::system_error (at co_await) on open failure (ENOENT, EACCES, …).
     */
    [[nodiscard]] static OpenFuture open(std::string path, FileMode mode);

    /**
     * @brief Single read from the current file position. Returns `{bytes_read, buf}`;
     * may return fewer bytes than `buf.size()`, and 0 at EOF.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     * @throws std::system_error (at co_await) on a read error.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] IoFuture<Buf> read(Buf buf);

    /**
     * @brief Loops until `buf.size()` bytes have been read or EOF is reached.
     * Returns `{bytes_read, buf}`; `bytes_read < buf.size()` indicates EOF.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] IoFuture<Buf> read_exact(Buf buf);

    /**
     * @brief Single read at `offset` (pread). Does not modify the file position.
     * Returns `{bytes_read, buf}`; may return fewer bytes than `buf.size()`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] IoFuture<Buf> read_at(Buf buf, int64_t offset);

    /**
     * @brief Loops at `offset` until `buf.size()` bytes have been read or EOF.
     * Does not modify the file position. Returns `{bytes_read, buf}`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] IoFuture<Buf> read_at_exact(Buf buf, int64_t offset);

    /**
     * @brief Single write to the current file position (the end, with `Append`).
     * Returns `{bytes_written, buf}`; may write fewer bytes than `buf.size()`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     * @throws std::system_error (at co_await) on a write error (e.g. ENOSPC).
     */
    template <ByteBuffer Buf>
    [[nodiscard]] IoFuture<Buf> write(Buf buf);

    /**
     * @brief Loops until all `buf.size()` bytes have been written.
     * Returns `{bytes_written, buf}`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] IoFuture<Buf> write_exact(Buf buf);

    /**
     * @brief Single write at `offset` (pwrite). Does not modify the file position.
     * Returns `{bytes_written, buf}`; may write fewer bytes than `buf.size()`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] IoFuture<Buf> write_at(Buf buf, int64_t offset);

    /**
     * @brief Loops at `offset` until all `buf.size()` bytes have been written.
     * Does not modify the file position. Returns `{bytes_written, buf}`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] IoFuture<Buf> write_at_exact(Buf buf, int64_t offset);

    /**
     * @brief Flushes the file's data and metadata to the device (fsync).
     * @throws std::system_error (at co_await) on failure, including a write error the
     *         kernel deferred (e.g. EIO, ENOSPC on some filesystems).
     */
    [[nodiscard]] BlockingHandle<void> sync_all();

private:
    explicit File(std::shared_ptr<detail::FileState> state) noexcept
        : m_state(std::move(state)) {}

    /// The shared body of every read and write: one job, one syscall (or a loop of them
    /// when `Exact`). `offset < 0` means the file position.
    template <bool Write, bool Exact, ByteBuffer Buf>
    IoFuture<Buf> transfer(Buf buf, int64_t offset, const char* what);

    /// The state, or std::logic_error naming `what` for a moved-from File.
    const std::shared_ptr<detail::FileState>& state(const char* what) const;

    std::shared_ptr<detail::FileState> m_state;
};

} // namespace coro

#include <coro/io/file.hpp>
