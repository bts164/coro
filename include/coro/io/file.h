#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/task/join_handle.h>
#include <uv.h>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

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
    // -----------------------------------------------------------------------
    // Future types
    //
    // ReadFuture and WriteFuture are JoinHandle<std::size_t> aliases — safe
    // to declare here since std::size_t is always complete.
    //
    // OpenFuture wraps JoinHandle<File>. Because File is incomplete inside
    // its own class body, OpenFuture is forward-declared here and defined
    // below after the closing brace.
    // -----------------------------------------------------------------------

    using OpenFuture  = JoinHandle<File>;
    using ReadFuture  = JoinHandle<std::size_t>;
    using WriteFuture = JoinHandle<std::size_t>;

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
     * @brief Reads up to `buf.size()` bytes from the current file position.
     * @return A `ReadFuture` that resolves to the number of bytes read, or 0 on EOF.
     * The buffer must remain valid until the future resolves.
     */
    [[nodiscard]] ReadFuture read(std::span<std::byte> buf);

    /**
     * @brief Reads up to `buf.size()` bytes starting at `offset` in the file.
     * Does not modify the file's current position (pread-style).
     * The buffer must remain valid until the future resolves.
     */
    [[nodiscard]] ReadFuture read_at(std::span<std::byte> buf, int64_t offset);

    /**
     * @brief Writes all of `data` to the current file position.
     * @return A `WriteFuture` that resolves to the number of bytes written.
     * The data buffer must remain valid until the future resolves.
     */
    [[nodiscard]] WriteFuture write(std::span<const std::byte> data);

    /**
     * @brief Writes all of `data` starting at `offset` in the file.
     * Does not modify the file's current position (pwrite-style).
     * The data buffer must remain valid until the future resolves.
     */
    [[nodiscard]] WriteFuture write_at(std::span<const std::byte> data, int64_t offset);

private:
    explicit File(uv_file fd, SingleThreadedUvExecutor* exec);

    uv_file                   m_fd   = -1;
    SingleThreadedUvExecutor* m_exec = nullptr;
};

} // namespace coro
