#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/io/byte_buffer.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/task/join_handle.h>
#include <uv.h>
#include <cstddef>
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

// ---------------------------------------------------------------------------
// File
// ---------------------------------------------------------------------------

/**
 * @brief Async file handle backed by libuv's thread-pool file I/O.
 *
 * Obtain a `File` via `co_await File::open(path, mode)`. Once opened, use
 * `read()`, `write()`, `read_at()`, `write_at()` to transfer data. The
 * destructor closes the file asynchronously on the uv executor.
 *
 * All I/O operations internally use `with_context` to run on the
 * @ref SingleThreadedUvExecutor, using stack-allocated @ref UvCallbackResult
 * objects awaited via @ref wait(). The coroutine that calls `read()` or
 * `write()` need not be on the uv executor — the future transparently
 * migrates work there and back.
 *
 * **Concurrency:** a `File` must not be shared across tasks. Only one read
 * or write future may be in flight at a time per file descriptor.
 *
 * **libuv fd recycling hazard:** the destructor closes the file
 * asynchronously. Avoid opening the same path from another task until the
 * close completes.
 */
class File {
public:
    using OpenFuture = JoinHandle<File>;

    // -----------------------------------------------------------------------
    // File public API
    // -----------------------------------------------------------------------

    File(File&&) noexcept;
    File& operator=(File&&) noexcept;
    File(const File&)            = delete;
    File& operator=(const File&) = delete;

    /// Closes the file asynchronously on the uv executor. Does not block.
    ~File();

    /**
     * @brief Opens a file at `path` with the given mode.
     * @return An `OpenFuture` that resolves to a `File` handle.
     * @throws std::system_error on open failure (ENOENT, EACCES, …).
     */
    [[nodiscard]] static OpenFuture open(std::string path, FileMode mode);

    /**
     * @brief Single read from the current file position. Returns `{bytes_read, buf}`;
     * may return fewer bytes than `buf.size()`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] JoinHandle<std::pair<std::size_t, Buf>> read(Buf buf);

    /**
     * @brief Loops until `buf.size()` bytes have been read or EOF is reached.
     * Returns `{bytes_read, buf}`; `bytes_read < buf.size()` indicates EOF.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] JoinHandle<std::pair<std::size_t, Buf>> read_exact(Buf buf);

    /**
     * @brief Single read at `offset` (pread-style). Does not modify the file position.
     * Returns `{bytes_read, buf}`; may return fewer bytes than `buf.size()`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] JoinHandle<std::pair<std::size_t, Buf>> read_at(Buf buf, int64_t offset);

    /**
     * @brief Loops at `offset` until `buf.size()` bytes have been read or EOF.
     * Does not modify the file position. Returns `{bytes_read, buf}`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] JoinHandle<std::pair<std::size_t, Buf>> read_at_exact(Buf buf, int64_t offset);

    /**
     * @brief Single write to the current file position. Returns `{bytes_written, buf}`;
     * may write fewer bytes than `buf.size()`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] JoinHandle<std::pair<std::size_t, Buf>> write(Buf buf);

    /**
     * @brief Loops until all `buf.size()` bytes have been written.
     * Returns `{bytes_written, buf}`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] JoinHandle<std::pair<std::size_t, Buf>> write_exact(Buf buf);

    /**
     * @brief Single write at `offset` (pwrite-style). Does not modify the file position.
     * Returns `{bytes_written, buf}`; may write fewer bytes than `buf.size()`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] JoinHandle<std::pair<std::size_t, Buf>> write_at(Buf buf, int64_t offset);

    /**
     * @brief Loops at `offset` until all `buf.size()` bytes have been written.
     * Does not modify the file position. Returns `{bytes_written, buf}`.
     * @tparam Buf Any type satisfying @ref ByteBuffer.
     */
    template <ByteBuffer Buf>
    [[nodiscard]] JoinHandle<std::pair<std::size_t, Buf>> write_at_exact(Buf buf, int64_t offset);

private:
    explicit File(uv_file fd, SingleThreadedUvExecutor* exec);

    uv_file                   m_fd   = -1;
    SingleThreadedUvExecutor* m_exec = nullptr;
};

} // namespace coro

#include <coro/io/file.hpp>
