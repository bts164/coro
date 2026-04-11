#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/runtime/io_service.h>
#include <uv.h>
#include <atomic>
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
 * destructor closes the file asynchronously via @ref IoService.
 *
 * **Concurrency:** a `File` must not be shared across tasks. Only one read or
 * write future may be in flight at a time per file descriptor.
 *
 * **Cancellation:** mid-operation cancellation (dropping a `ReadFuture` or
 * `WriteFuture` before it completes) uses `uv_cancel()` for best-effort
 * cancellation. The callback may still fire with `UV_ECANCELED`, but the
 * future's waker will not be invoked.
 *
 * **libuv fd recycling hazard:** the destructor closes the file asynchronously.
 * If the same path is re-opened by another task before the close completes,
 * the OS may recycle the file descriptor, causing operations to target the
 * wrong file. This is a user-level race — avoid opening the same file from
 * multiple tasks without synchronization.
 *
 * All libuv handles, request structs, and callbacks are private implementation
 * details of this class — @ref IoService has no knowledge of filesystem internals.
 */
class File {
private:
    // -----------------------------------------------------------------------
    // State structs — shared between futures and the I/O thread.
    //
    // libuv requires the `uv_fs_t` request struct to remain stable until the
    // callback fires, so each operation allocates its state on the heap and
    // holds it via shared_ptr (outlives the future if the future is destroyed).
    //
    // RACE: `waker` written by worker thread, read by I/O thread (callback).
    //       `result`/`complete` written by I/O thread before `complete=true`
    //       with release; worker reads after seeing `complete` with acquire.
    // -----------------------------------------------------------------------

    struct OpenState {
        uv_fs_t                                     req;    // passed to uv_fs_open; must be stable
        std::atomic<std::shared_ptr<detail::Waker>> waker;
        std::atomic<bool>                           complete{false};
        uv_file                                     result = -1;  // fd (>=0) or error code (<0)
    };

    struct ReadState {
        uv_fs_t                                     req;      // passed to uv_fs_read; must be stable
        uv_buf_t                                    buf_desc; // points into caller's buffer
        uv_file                                     fd;       // file descriptor
        int64_t                                     offset;   // read offset (-1 = current position)
        std::atomic<std::shared_ptr<detail::Waker>> waker;
        std::atomic<bool>                           complete{false};
        std::atomic<bool>                           cancelled{false};  // set by destructor
        bool                                        started = false;   // worker thread only
        ssize_t                                     result = 0;  // bytes read (>=0) or error (<0)
    };

    struct WriteState {
        uv_fs_t                                     req;      // passed to uv_fs_write; must be stable
        uv_buf_t                                    buf_desc; // points into caller's data
        uv_file                                     fd;       // file descriptor
        int64_t                                     offset;   // write offset (-1 = current position)
        std::atomic<std::shared_ptr<detail::Waker>> waker;
        std::atomic<bool>                           complete{false};
        std::atomic<bool>                           cancelled{false};  // set by destructor
        bool                                        started = false;   // worker thread only
        ssize_t                                     result = 0;  // bytes written (>=0) or error (<0)
    };

    // -----------------------------------------------------------------------
    // IoRequest subtypes — submitted via IoService::submit()
    // -----------------------------------------------------------------------

    struct OpenRequest : IoRequest {
        std::shared_ptr<OpenState> state;
        std::string                path;
        int                        flags;   // libuv O_* flags
        int                        mode;    // POSIX permissions (0644 default)

        OpenRequest(std::shared_ptr<OpenState> s, std::string p, int f, int m)
            : state(std::move(s)), path(std::move(p)), flags(f), mode(m) {}

        void execute(uv_loop_t* loop) override;
    };

    struct ReadRequest : IoRequest {
        std::shared_ptr<ReadState> state;

        explicit ReadRequest(std::shared_ptr<ReadState> s)
            : state(std::move(s)) {}

        void execute(uv_loop_t* loop) override;
    };

    struct WriteRequest : IoRequest {
        std::shared_ptr<WriteState> state;

        explicit WriteRequest(std::shared_ptr<WriteState> s)
            : state(std::move(s)) {}

        void execute(uv_loop_t* loop) override;
    };

    struct CloseRequest : IoRequest {
        uv_file fd;

        explicit CloseRequest(uv_file f) : fd(f) {}

        void execute(uv_loop_t* loop) override;
    };

    struct CancelRequest : IoRequest {
        uv_fs_t* req;  // pointer to the State::req field; valid as long as State is alive

        explicit CancelRequest(uv_fs_t* r) : req(r) {}

        void execute(uv_loop_t* loop) override;
    };

    // -----------------------------------------------------------------------
    // libuv callbacks — all run exclusively on the I/O thread.
    // -----------------------------------------------------------------------

    /// Fired by libuv when uv_fs_open completes.
    static void open_cb(uv_fs_t* req);

    /// Fired by libuv when uv_fs_read completes.
    static void read_cb(uv_fs_t* req);

    /// Fired by libuv when uv_fs_write completes.
    static void write_cb(uv_fs_t* req);

    /// Fired by libuv when uv_fs_close completes. No waker; cleanup only.
    static void close_cb(uv_fs_t* req);

public:
    // -----------------------------------------------------------------------
    // Public nested Future types
    // -----------------------------------------------------------------------

    /**
     * @brief Future<File> returned by @ref File::open().
     *
     * On first poll, submits an `OpenRequest` to `IoService` which calls
     * `uv_fs_open()` on the I/O thread. When the thread-pool completes the
     * open, `open_cb` wakes this future. The next poll constructs the `File`.
     *
     * @throws std::system_error on open failure (wraps the libuv error code).
     */
    class OpenFuture {
    public:
        using OutputType = File;

        OpenFuture(std::string path, FileMode mode, IoService* io_service);

        OpenFuture(OpenFuture&&) noexcept            = default;
        OpenFuture& operator=(OpenFuture&&) noexcept = default;
        OpenFuture(const OpenFuture&)                = delete;
        OpenFuture& operator=(const OpenFuture&)     = delete;

        PollResult<File> poll(detail::Context& ctx);

    private:
        std::string                m_path;
        FileMode                   m_mode;
        IoService*                 m_io_service;
        std::shared_ptr<OpenState> m_state;  // null until first poll
    };

    /**
     * @brief Future<std::size_t> returned by @ref File::read() / read_at().
     *
     * On first poll, submits a `ReadRequest` which calls `uv_fs_read()`.
     * When the thread-pool completes the read, `read_cb` wakes the future.
     * Returns the number of bytes read, or 0 on EOF.
     *
     * **Cancellation:** dropping this future before it completes submits a
     * cancel request via `uv_cancel()`. The buffer must remain valid until
     * the future resolves or is destroyed.
     *
     * @throws std::system_error on read failure (wraps the libuv error code).
     */
    class ReadFuture {
    public:
        using OutputType = std::size_t;

        ReadFuture(uv_file           fd,
                   std::span<std::byte> buf,
                   int64_t           offset,
                   IoService*        io_service);

        ReadFuture(ReadFuture&&) noexcept;
        ReadFuture& operator=(ReadFuture&&) noexcept = default;
        ReadFuture(const ReadFuture&)                = delete;
        ReadFuture& operator=(const ReadFuture&)     = delete;

        ~ReadFuture();  // submits cancel request if in-flight

        PollResult<std::size_t> poll(detail::Context& ctx);

    private:
        IoService*                 m_io_service;
        std::shared_ptr<ReadState> m_state;
    };

    /**
     * @brief Future<std::size_t> returned by @ref File::write() / write_at().
     *
     * On first poll, submits a `WriteRequest` which calls `uv_fs_write()`.
     * When the thread-pool completes the write, `write_cb` wakes the future.
     * Returns the number of bytes written (libuv guarantees full write or error).
     *
     * **Cancellation:** dropping this future before it completes submits a
     * cancel request via `uv_cancel()`. The data buffer must remain valid
     * until the future resolves or is destroyed.
     *
     * @throws std::system_error on write failure (wraps the libuv error code).
     */
    class WriteFuture {
    public:
        using OutputType = std::size_t;

        WriteFuture(uv_file                    fd,
                    std::span<const std::byte> data,
                    int64_t                    offset,
                    IoService*                 io_service);

        WriteFuture(WriteFuture&&) noexcept;
        WriteFuture& operator=(WriteFuture&&) noexcept = default;
        WriteFuture(const WriteFuture&)                = delete;
        WriteFuture& operator=(const WriteFuture&)     = delete;

        ~WriteFuture();  // submits cancel request if in-flight

        PollResult<std::size_t> poll(detail::Context& ctx);

    private:
        IoService*                  m_io_service;
        std::shared_ptr<WriteState> m_state;
    };

    // -----------------------------------------------------------------------
    // File public API
    // -----------------------------------------------------------------------

    File(File&&) noexcept;
    File& operator=(File&&) noexcept;
    File(const File&)            = delete;
    File& operator=(const File&) = delete;

    /// Closes the file asynchronously via IoService. Does not block.
    ~File();

    /**
     * @brief Opens a file at `path` with the given mode.
     * @return An `OpenFuture` that resolves to a `File` handle.
     *
     * @throws std::system_error on open failure (ENOENT, EACCES, etc.).
     */
    [[nodiscard]] static OpenFuture open(std::string path, FileMode mode);

    /**
     * @brief Reads up to `buf.size()` bytes from the current file position.
     * @return A `ReadFuture` that resolves to the number of bytes read, or 0 on EOF.
     *
     * **Note:** the buffer must remain valid until the future resolves.
     */
    [[nodiscard]] ReadFuture read(std::span<std::byte> buf);

    /**
     * @brief Reads up to `buf.size()` bytes starting at `offset` in the file.
     * @return A `ReadFuture` that resolves to the number of bytes read, or 0 on EOF.
     *
     * Does not modify the file's current position (pread-style).
     *
     * **Note:** the buffer must remain valid until the future resolves.
     */
    [[nodiscard]] ReadFuture read_at(std::span<std::byte> buf, int64_t offset);

    /**
     * @brief Writes all of `data` to the current file position.
     * @return A `WriteFuture` that resolves to the number of bytes written.
     *
     * **Note:** the data buffer must remain valid until the future resolves.
     */
    [[nodiscard]] WriteFuture write(std::span<const std::byte> data);

    /**
     * @brief Writes all of `data` starting at `offset` in the file.
     * @return A `WriteFuture` that resolves to the number of bytes written.
     *
     * Does not modify the file's current position (pwrite-style).
     *
     * **Note:** the data buffer must remain valid until the future resolves.
     */
    [[nodiscard]] WriteFuture write_at(std::span<const std::byte> data, int64_t offset);

private:
    explicit File(uv_file fd, IoService* io_service);

    uv_file    m_fd = -1;
    IoService* m_io_service = nullptr;
};

} // namespace coro
