# File I/O

## Overview

This document describes the design for async filesystem read/write primitives built on top of
libuv's thread-pool file I/O. The library already has `IoService` infrastructure and async
TCP/WebSocket streams; this feature extends the same pattern to files.

Unlike network I/O (which is truly async at the OS level via epoll/kqueue), filesystem
operations in libuv use a **thread pool** — `uv_fs_*` calls are dispatched to worker threads
and notify the event loop when complete. From the caller's perspective this is still
non-blocking: the coroutine suspends and yields the executor thread while the file operation
runs in the background.

---

## Goals

- **Provide `File` — an async file handle** with `read()`, `write()`, `close()` operations
  that return `Future`s, composing naturally with the rest of the library.
- **Follow the existing `TcpStream` pattern** — all libuv-specific types (`uv_fs_t`,
  `uv_file`, callbacks) are private to `File`, not exposed in `IoService`.
- **Support cancellation** — dropping a `ReadFuture` or `WriteFuture` mid-operation must
  cancel the pending libuv request safely (libuv provides `uv_cancel` for this).
- **Match libuv's guarantees** — operations are ordered per-file-descriptor but may be
  reordered across descriptors by the thread pool.

---

## Non-Goals (Future Work)

The initial implementation focuses on the core read/write path. The following are deferred:

- **High-level file utilities** — `read_to_string()`, `write_all()`, buffered readers/writers.
  These can be built on top of the low-level `File` primitives.
- **Directory operations** — `readdir()`, `mkdir()`, `stat()`, etc. These follow the same
  pattern but are a separate feature.
- **Seeking** — `seek()` / `tell()` can be added once the core read/write path is validated.
- **Memory-mapped I/O** — libuv does not provide async mmap; this would require platform-specific
  code outside of libuv.

---

## User-Facing API

```cpp
#include <coro/io/file.h>

coro::Coro<void> example() {
    // Open a file for reading
    auto file = co_await coro::File::open("data.txt", coro::FileMode::Read);

    // Read up to 4096 bytes
    std::array<std::byte, 4096> buf;
    std::size_t n = co_await file.read(std::span(buf));

    // Write to a file
    auto out = co_await coro::File::open("output.txt",
                                         coro::FileMode::Write | coro::FileMode::Create);
    co_await out.write(std::span(buf).subspan(0, n));

    // Files are closed automatically in the destructor (async via IoService)
}
```

### `FileMode` flags

```cpp
enum class FileMode : unsigned {
    Read       = 0x01,  // O_RDONLY
    Write      = 0x02,  // O_WRONLY
    ReadWrite  = 0x03,  // O_RDWR
    Create     = 0x10,  // O_CREAT  — create if not exists
    Truncate   = 0x20,  // O_TRUNC  — truncate to zero length on open
    Append     = 0x40,  // O_APPEND — writes always go to end
};
```

Users combine flags with `|`: `FileMode::Write | FileMode::Create | FileMode::Truncate`.

---

## `File` Class Design

`File` is move-only and owns a libuv file descriptor (`uv_file`). The destructor closes the
descriptor asynchronously via a `CloseRequest` submitted to `IoService`, matching the
`TcpStream` close pattern.

All libuv request structs (`uv_fs_t`), state objects, and callbacks are **private
implementation details** of `File`. `IoService` has no knowledge of filesystem operations —
it only knows `IoRequest::execute()`.

```cpp
// include/coro/io/file.h

namespace coro {

class File {
private:
    // -----------------------------------------------------------------------
    // State structs — shared between futures and the I/O thread.
    //
    // libuv requires the `uv_fs_t` request struct to remain stable until the
    // callback fires, so each operation allocates its state on the heap and
    // holds it via shared_ptr (outlives the future if the future is destroyed).
    // -----------------------------------------------------------------------

    struct OpenState {
        uv_fs_t                                     req;    // passed to uv_fs_open; must be stable
        std::atomic<std::shared_ptr<detail::Waker>> waker;
        std::atomic<bool>                           complete{false};
        uv_file                                     result = -1;  // fd or error code
    };

    struct ReadState {
        uv_fs_t                                     req;    // passed to uv_fs_read; must be stable
        uv_buf_t                                    buf_desc; // points into caller's buffer
        std::atomic<std::shared_ptr<detail::Waker>> waker;
        std::atomic<bool>                           complete{false};
        std::atomic<bool>                           cancelled{false};  // set by destructor
        bool                                        started = false;   // worker thread only
        ssize_t                                     result = 0;  // bytes read or error code
    };

    struct WriteState {
        uv_fs_t                                     req;    // passed to uv_fs_write; must be stable
        uv_buf_t                                    buf_desc; // points into caller's data
        std::atomic<std::shared_ptr<detail::Waker>> waker;
        std::atomic<bool>                           complete{false};
        std::atomic<bool>                           cancelled{false};
        bool                                        started = false;   // worker thread only
        ssize_t                                     result = 0;  // bytes written or error code
    };

    // -----------------------------------------------------------------------
    // IoRequest subtypes — submitted via IoService::submit()
    // -----------------------------------------------------------------------

    struct OpenRequest : IoRequest {
        std::shared_ptr<OpenState> state;
        std::string                path;
        int                        flags;   // libuv O_* flags
        int                        mode;    // POSIX permissions (0644 default)

        void execute(uv_loop_t* loop) override;
    };

    struct ReadRequest : IoRequest {
        std::shared_ptr<ReadState> state;
        uv_file                    fd;
        int64_t                    offset;  // -1 = use current file position

        void execute(uv_loop_t* loop) override;
    };

    struct WriteRequest : IoRequest {
        std::shared_ptr<WriteState> state;
        uv_file                     fd;
        int64_t                     offset;  // -1 = use current file position

        void execute(uv_loop_t* loop) override;
    };

    struct CloseRequest : IoRequest {
        uv_file fd;

        void execute(uv_loop_t* loop) override;
    };

    // -----------------------------------------------------------------------
    // libuv callbacks — all run on the I/O thread.
    // -----------------------------------------------------------------------

    static void open_cb(uv_fs_t* req);
    static void read_cb(uv_fs_t* req);
    static void write_cb(uv_fs_t* req);
    static void close_cb(uv_fs_t* req);  // cleanup only; no waker

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
        std::string                  m_path;
        FileMode                     m_mode;
        IoService*                   m_io_service;
        std::shared_ptr<OpenState>   m_state;  // null until first poll
    };

    /**
     * @brief Future<std::size_t> returned by @ref File::read().
     *
     * On first poll, submits a `ReadRequest` which calls `uv_fs_read()`.
     * When the thread-pool completes the read, `read_cb` wakes the future.
     * Returns the number of bytes read, or 0 on EOF.
     *
     * **Cancellation:** dropping this future before it completes submits a
     * cancel request via `uv_cancel()`. The buffer must remain valid until
     * the future resolves or is destroyed.
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
     * @brief Future<std::size_t> returned by @ref File::write().
     *
     * On first poll, submits a `WriteRequest` which calls `uv_fs_write()`.
     * When the thread-pool completes the write, `write_cb` wakes the future.
     * Returns the number of bytes written (libuv guarantees full write or error).
     *
     * **Cancellation:** dropping this future before it completes submits a
     * cancel request via `uv_cancel()`. The data buffer must remain valid
     * until the future resolves or is destroyed.
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
     */
    [[nodiscard]] static OpenFuture open(std::string path, FileMode mode);

    /**
     * @brief Reads up to `buf.size()` bytes from the current file position.
     * @return A `ReadFuture` that resolves to the number of bytes read, or 0 on EOF.
     */
    [[nodiscard]] ReadFuture read(std::span<std::byte> buf);

    /**
     * @brief Reads up to `buf.size()` bytes starting at `offset` in the file.
     * @return A `ReadFuture` that resolves to the number of bytes read, or 0 on EOF.
     *
     * Does not modify the file's current position (pread-style).
     */
    [[nodiscard]] ReadFuture read_at(std::span<std::byte> buf, int64_t offset);

    /**
     * @brief Writes all of `data` to the current file position.
     * @return A `WriteFuture` that resolves to the number of bytes written.
     */
    [[nodiscard]] WriteFuture write(std::span<const std::byte> data);

    /**
     * @brief Writes all of `data` starting at `offset` in the file.
     * @return A `WriteFuture` that resolves to the number of bytes written.
     *
     * Does not modify the file's current position (pwrite-style).
     */
    [[nodiscard]] WriteFuture write_at(std::span<const std::byte> data, int64_t offset);

private:
    explicit File(uv_file fd, IoService* io_service);

    uv_file    m_fd = -1;
    IoService* m_io_service = nullptr;
};

} // namespace coro
```

---

## Implementation Details

### libuv Filesystem API Primer

libuv's filesystem API is callback-based and backed by a thread pool:

```c
// Open a file:
int uv_fs_open(uv_loop_t* loop, uv_fs_t* req, const char* path,
               int flags, int mode, uv_fs_cb cb);

// Read from a file:
int uv_fs_read(uv_loop_t* loop, uv_fs_t* req, uv_file file,
               const uv_buf_t bufs[], unsigned int nbufs, int64_t offset,
               uv_fs_cb cb);

// Write to a file:
int uv_fs_write(uv_loop_t* loop, uv_fs_t* req, uv_file file,
                const uv_buf_t bufs[], unsigned int nbufs, int64_t offset,
                uv_fs_cb cb);

// Close a file:
int uv_fs_close(uv_loop_t* loop, uv_fs_t* req, uv_file file, uv_fs_cb cb);

// Cleanup after a request completes:
void uv_fs_req_cleanup(uv_fs_t* req);
```

All callbacks fire on the event loop thread (the `IoService` I/O thread in our design).
The `uv_fs_t*` request struct must remain stable until the callback fires — hence
`shared_ptr<State>` to outlive the future if it is destroyed.

### Cancellation

libuv provides `uv_cancel((uv_req_t*)req)` to cancel in-flight filesystem requests.
Cancellation is **best-effort**: if the thread-pool worker has already started the
operation, it cannot be stopped. The callback will still fire, but with result = `UV_ECANCELED`.

Our cancellation protocol:

1. `~ReadFuture()` / `~WriteFuture()` set `state->cancelled = true` (atomic).
2. If `started == true`, submit a `CancelRequest` to `IoService` which calls `uv_cancel()`.
3. `read_cb` / `write_cb` check `state->cancelled` after acquiring `complete`:
   - If cancelled, do **not** wake the future (it is already destroyed).
   - If not cancelled, call `waker->wake()` as usual.

This mirrors the `SleepFuture` cancellation pattern but adds the `uv_cancel()` call.

### Open Flags Translation

`FileMode` is a user-friendly enum; `OpenRequest::execute()` translates it to libuv flags:

```cpp
int translate_flags(FileMode mode) {
    int flags = 0;
    if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite)
        flags |= O_RDWR;
    else if (mode & FileMode::Write)
        flags |= O_WRONLY;
    else
        flags |= O_RDONLY;

    if (mode & FileMode::Create)   flags |= O_CREAT;
    if (mode & FileMode::Truncate) flags |= O_TRUNC;
    if (mode & FileMode::Append)   flags |= O_APPEND;

    return flags;
}
```

The `mode` argument to `uv_fs_open` (POSIX permissions) defaults to `0644` (owner rw,
group/other read-only) when `Create` is set, and is ignored otherwise.

### Offset Handling

- `read()` / `write()` pass `-1` as the offset, telling libuv to use and update the file's
  current position (like POSIX `read()`/`write()`).
- `read_at()` / `write_at()` pass an explicit offset, implementing pread/pwrite semantics
  (position is not modified).

---

## Concurrency Hazards

All known concurrency concerns are listed here for validation during Phase 2 and 3.

### RESOLVED — `State::waker` concurrent read/write
Worker thread writes `waker` on re-poll; I/O thread reads it in the callback. Resolved by
`std::atomic<std::shared_ptr<Waker>>` in all state structs.

### RESOLVED — `State` freed before the callback fires
If the future is destroyed before the I/O thread processes the callback, the state must
remain alive. Resolved by each `IoRequest` subclass holding `shared_ptr<State>`.

### RESOLVED — `cancelled` read without the lock
`~ReadFuture` / `~WriteFuture` write `cancelled`; the callback reads it. Resolved by
making `cancelled` `std::atomic<bool>`.

### CAUTION — `uv_cancel()` may fire the callback with `UV_ECANCELED`
The callback must distinguish "cancelled by us" from "failed naturally". Solution:
check `state->cancelled` in the callback. If true, do not wake the future (it is gone).
If false and `req->result == UV_ECANCELED`, treat it as an I/O error and wake normally.

### CAUTION — `uv_fs_req_cleanup()` must be called in every callback
libuv allocates internal memory for the path string (in `uv_fs_open`) and other metadata.
`uv_fs_req_cleanup(&state->req)` must be the **first** line in every callback, before
accessing `req->result`. Failure to call this leaks memory.

### CAUTION — `CloseRequest` has no callback or state
The destructor submits a `CloseRequest` but does not wait for the close to complete —
the close happens asynchronously after the `File` is destroyed. This is safe because
the libuv file descriptor is an integer, not a pointer, so there is no UAF risk. However,
if the same file is re-opened before the close completes, the OS may recycle the descriptor,
leading to operations on the wrong file. This is a user-level race (opening the same path
from multiple tasks) — not preventable at the library level. Document this in the `File`
class comment.

### CAUTION — `File::open()` must be called from an executor context
`OpenFuture::poll()` calls `current_io_service()`, which throws if no `IoService` is set
on the current thread. Users must not call `File::open()` outside of `Runtime::block_on()`
or a spawned task. This is the same constraint as `SleepFuture` and `TcpStream` — enforce
via documentation and the existing `current_io_service()` check.

---

## Testing Strategy (Phase 2 / 3)

### Unit Tests

Create `test/test_file.cpp` with the following test cases:

1. **Basic open/read/write/close** — open a temp file, write data, close, re-open read-only,
   read back, verify contents match.
2. **Read EOF** — read past the end of a file; verify `read()` returns 0.
3. **Write to a file opened without `Create`** — expect open to fail with `ENOENT`.
4. **Read from a file opened write-only** — expect read to fail with `EBADF`.
5. **Positional I/O** — write at offset 100, read at offset 100, verify data.
6. **Large file** — write 10MB in a loop, read back in chunks, verify correctness.
7. **Cancellation** — start a read on a large file, drop the future before it completes,
   verify no crash and no callback fires (check with a canary flag).
8. **Multiple files concurrently** — open 10 files in parallel, write to all, read from all,
   verify no cross-contamination.

All tests use the existing `Runtime::block_on()` harness.

---

## Example Usage

### Simple read

```cpp
coro::Coro<std::string> read_file(std::string path) {
    auto file = co_await coro::File::open(std::move(path), coro::FileMode::Read);

    std::string contents;
    std::array<std::byte, 4096> buf;
    while (true) {
        std::size_t n = co_await file.read(std::span(buf));
        if (n == 0) break;  // EOF
        contents.append(reinterpret_cast<const char*>(buf.data()), n);
    }

    co_return contents;
}
```

### Write with error handling

```cpp
coro::Coro<void> write_log(std::string message) {
    auto file = co_await coro::File::open("app.log",
        coro::FileMode::Write | coro::FileMode::Create | coro::FileMode::Append);

    auto data = std::as_bytes(std::span(message));
    std::size_t written = co_await file.write(data);

    if (written != data.size()) {
        throw std::runtime_error("partial write");
    }
}
```

---

## File Placement

Following `doc/module_structure.md`:

- **Header:** `include/coro/io/file.h` (alongside `tcp_stream.h`, `ws_stream.h`)
- **Implementation:** `src/io/file.cpp`
- **Tests:** `test/test_file.cpp`

---

## Dependencies

No new dependencies — libuv is already integrated. The filesystem API is part of the core
libuv library; no additional Conan packages are needed.

---

## Open Questions

### Should `File::open()` accept a free function or an OpenFuture factory?

Current design:
```cpp
auto file = co_await File::open(path, mode);  // static method returning OpenFuture
```

Alternative (factory pattern):
```cpp
auto file = co_await coro::open_file(path, mode);  // free function returning OpenFuture
```

**Decision:** stick with the static method. It matches `TcpStream::connect()` and keeps
the API surface on the `File` type itself rather than polluting the `coro::` namespace
with free functions.

### Should `FileMode` be a class enum or plain flags?

Current design uses `enum class FileMode : unsigned` with bitwise operators. Alternatives:
- Plain `enum` (pollutes the `coro::` namespace with `Read`, `Write`, etc.)
- A `struct FileMode { bool read; bool write; ... }` (verbose, no natural `|` syntax)

**Decision:** `enum class` with `operator|` overload is the most ergonomic. Users write
`FileMode::Write | FileMode::Create`, which is self-documenting.

### Should reads/writes return `std::expected<size_t, std::error_code>` or throw?

The roadmap item "Migrate error-returning futures to `std::expected`" is planned but not
yet implemented. For consistency with `TcpStream`, the initial `File` implementation will
**throw** `std::system_error` on I/O errors. Once the `std::expected` migration lands,
`File` will be updated to match.

**Decision:** throw for now, migrate to `std::expected` in a follow-on pass.

---

## Summary

This design extends the existing `IoService` pattern to filesystem I/O. `File` follows the
same structure as `TcpStream`: all libuv-specific state is private, operations return
`Future`s, and cancellation is handled via atomic flags + `uv_cancel()`. The implementation
fits cleanly into the existing runtime without changes to `IoService`, `Executor`, or
`Waker`.
