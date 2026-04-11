#include <coro/io/file.h>
#include <coro/detail/poll_result.h>
#include <system_error>
#include <fcntl.h>

namespace coro {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Throws std::system_error for a libuv error code.
[[noreturn]] void throw_uv_error(int status, const char* what) {
    throw std::system_error(
        std::error_code(-status, std::system_category()),
        what);
}

} // namespace

// ---------------------------------------------------------------------------
// Helper: translate FileMode to libuv flags
// ---------------------------------------------------------------------------

static int translate_flags(FileMode mode) {
    int flags = 0;

    // Access mode
    if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite)
        flags |= O_RDWR;
    else if ((mode & FileMode::Write) != FileMode{})
        flags |= O_WRONLY;
    else
        flags |= O_RDONLY;

    // Creation/truncation flags
    if ((mode & FileMode::Create) != FileMode{})   flags |= O_CREAT;
    if ((mode & FileMode::Truncate) != FileMode{}) flags |= O_TRUNC;
    if ((mode & FileMode::Append) != FileMode{})   flags |= O_APPEND;

    return flags;
}

// ---------------------------------------------------------------------------
// IoRequest::execute() implementations
// ---------------------------------------------------------------------------

void File::OpenRequest::execute(uv_loop_t* loop) {
    // Store a heap-allocated shared_ptr in req.data so open_cb can access
    // the state. open_cb deletes the wrapper after use.
    state->req.data = new std::shared_ptr<OpenState>(state);

    int r = uv_fs_open(loop, &state->req, path.c_str(), flags, mode, open_cb);
    if (r != 0) {
        // Synchronous failure
        delete static_cast<std::shared_ptr<OpenState>*>(state->req.data);
        state->req.data = nullptr;
        state->result = r;
        state->complete.store(true, std::memory_order_release);
        if (auto w = state->waker.load())
            w->wake();
    }
}

void File::ReadRequest::execute(uv_loop_t* loop) {
    // Store a heap-allocated shared_ptr in req.data so read_cb can find it
    state->req.data = new std::shared_ptr<ReadState>(state);

    int r = uv_fs_read(loop, &state->req, state->fd, &state->buf_desc, 1, state->offset, read_cb);
    if (r != 0) {
        // Synchronous failure
        delete static_cast<std::shared_ptr<ReadState>*>(state->req.data);
        state->req.data = nullptr;
        state->result = r;
        state->complete.store(true, std::memory_order_release);
        if (auto w = state->waker.load())
            w->wake();
    }
}

void File::WriteRequest::execute(uv_loop_t* loop) {
    // Store a heap-allocated shared_ptr in req.data so write_cb can find it
    state->req.data = new std::shared_ptr<WriteState>(state);

    int r = uv_fs_write(loop, &state->req, state->fd, &state->buf_desc, 1, state->offset, write_cb);
    if (r != 0) {
        // Synchronous failure
        delete static_cast<std::shared_ptr<WriteState>*>(state->req.data);
        state->req.data = nullptr;
        state->result = r;
        state->complete.store(true, std::memory_order_release);
        if (auto w = state->waker.load())
            w->wake();
    }
}

void File::CloseRequest::execute(uv_loop_t* loop) {
    // Close is fire-and-forget — no state to track, callback is just for cleanup
    uv_fs_t req;
    uv_fs_close(loop, &req, fd, close_cb);
}

void File::CancelRequest::execute(uv_loop_t* loop) {
    (void)loop;
    // Best-effort cancellation
    uv_cancel(reinterpret_cast<uv_req_t*>(req));
}

// ---------------------------------------------------------------------------
// libuv callbacks
// ---------------------------------------------------------------------------

void File::open_cb(uv_fs_t* req) {
    // Recover the state from req.data
    auto state_ptr = static_cast<std::shared_ptr<OpenState>*>(req->data);
    auto state = *state_ptr;
    delete state_ptr;

    // Clean up libuv request internals
    state->result = static_cast<uv_file>(req->result);
    uv_fs_req_cleanup(req);

    // Signal completion and wake the future
    state->complete.store(true, std::memory_order_release);
    if (auto w = state->waker.load())
        w->wake();
}

void File::read_cb(uv_fs_t* req) {
    // Recover the state from req.data
    auto state_ptr = static_cast<std::shared_ptr<ReadState>*>(req->data);
    auto state = *state_ptr;
    delete state_ptr;

    // Check if the future was cancelled before we got here
    bool cancelled = state->cancelled.load(std::memory_order_acquire);

    // Store the result
    state->result = req->result;
    uv_fs_req_cleanup(req);

    // Signal completion
    state->complete.store(true, std::memory_order_release);

    // Only wake if not cancelled (future may be destroyed)
    if (!cancelled) {
        if (auto w = state->waker.load())
            w->wake();
    }
}

void File::write_cb(uv_fs_t* req) {
    // Recover the state from req.data
    auto state_ptr = static_cast<std::shared_ptr<WriteState>*>(req->data);
    auto state = *state_ptr;
    delete state_ptr;

    // Check if the future was cancelled before we got here
    bool cancelled = state->cancelled.load(std::memory_order_acquire);

    // Store the result
    state->result = req->result;
    uv_fs_req_cleanup(req);

    // Signal completion
    state->complete.store(true, std::memory_order_release);

    // Only wake if not cancelled (future may be destroyed)
    if (!cancelled) {
        if (auto w = state->waker.load())
            w->wake();
    }
}

void File::close_cb(uv_fs_t* req) {
    // Cleanup only — no waker to call
    uv_fs_req_cleanup(req);
}

// ---------------------------------------------------------------------------
// OpenFuture
// ---------------------------------------------------------------------------

File::OpenFuture::OpenFuture(std::string path, FileMode mode, IoService* io_service)
    : m_path(std::move(path)), m_mode(mode), m_io_service(io_service) {}

PollResult<File> File::OpenFuture::poll(detail::Context& ctx) {
    if (!m_state) {
        // First poll — allocate state and submit open request
        m_state = std::make_shared<OpenState>();

        int flags = translate_flags(m_mode);
        int posix_mode = 0644;  // Default file permissions

        m_io_service->submit(std::make_unique<OpenRequest>(
            m_state, m_path, flags, posix_mode));

        // Store the waker for the I/O thread to call
        m_state->waker.store(ctx.getWaker());
        return PollPending;
    }

    // Check if the operation completed
    if (!m_state->complete.load(std::memory_order_acquire)) {
        // Still pending — update waker in case we're in a new executor context
        m_state->waker.store(ctx.getWaker());
        return PollPending;
    }

    // Operation completed — check result
    uv_file fd = m_state->result;
    if (fd < 0) {
        // Open failed
        throw_uv_error(static_cast<int>(fd), "uv_fs_open");
    }

    // Success — construct and return the File
    return File(fd, m_io_service);
}

// ---------------------------------------------------------------------------
// ReadFuture
// ---------------------------------------------------------------------------

File::ReadFuture::ReadFuture(uv_file              fd,
                             std::span<std::byte> buf,
                             int64_t              offset,
                             IoService*           io_service)
    : m_io_service(io_service) {
    // Allocate state and set up all fields
    m_state = std::make_shared<ReadState>();
    m_state->fd = fd;
    m_state->offset = offset;
    m_state->buf_desc = uv_buf_init(
        reinterpret_cast<char*>(buf.data()),
        static_cast<unsigned int>(buf.size()));
}

File::ReadFuture::ReadFuture(ReadFuture&&) noexcept = default;

File::ReadFuture::~ReadFuture() {
    if (m_state && m_state->started && !m_state->complete.load(std::memory_order_acquire)) {
        // Operation is in flight — cancel it
        m_state->cancelled.store(true, std::memory_order_release);
        m_io_service->submit(std::make_unique<CancelRequest>(&m_state->req));
    }
}

PollResult<std::size_t> File::ReadFuture::poll(detail::Context& ctx) {
    if (!m_state->started) {
        // First poll — submit read request
        m_state->started = true;
        m_state->waker.store(ctx.getWaker());

        m_io_service->submit(std::make_unique<ReadRequest>(m_state));
        return PollPending;
    }

    // Check if the operation completed
    if (!m_state->complete.load(std::memory_order_acquire)) {
        // Still pending — update waker
        m_state->waker.store(ctx.getWaker());
        return PollPending;
    }

    // Check for cancellation
    if (m_state->cancelled.load(std::memory_order_acquire)) {
        throw std::runtime_error("Read operation cancelled");
    }

    // Operation completed — check result
    ssize_t result = m_state->result;
    if (result < 0) {
        // Read failed
        throw_uv_error(static_cast<int>(result), "uv_fs_read");
    }

    // Success — return bytes read (0 = EOF)
    return static_cast<std::size_t>(result);
}

// ---------------------------------------------------------------------------
// WriteFuture
// ---------------------------------------------------------------------------

File::WriteFuture::WriteFuture(uv_file                    fd,
                               std::span<const std::byte> data,
                               int64_t                    offset,
                               IoService*                 io_service)
    : m_io_service(io_service) {
    // Allocate state and set up all fields
    m_state = std::make_shared<WriteState>();
    m_state->fd = fd;
    m_state->offset = offset;
    m_state->buf_desc = uv_buf_init(
        // libuv expects a non-const char*, but won't modify it for writes
        const_cast<char*>(reinterpret_cast<const char*>(data.data())),
        static_cast<unsigned int>(data.size()));
}

File::WriteFuture::WriteFuture(WriteFuture&&) noexcept = default;

File::WriteFuture::~WriteFuture() {
    if (m_state && m_state->started && !m_state->complete.load(std::memory_order_acquire)) {
        // Operation is in flight — cancel it
        m_state->cancelled.store(true, std::memory_order_release);
        m_io_service->submit(std::make_unique<CancelRequest>(&m_state->req));
    }
}

PollResult<std::size_t> File::WriteFuture::poll(detail::Context& ctx) {
    if (!m_state->started) {
        // First poll — submit write request
        m_state->started = true;
        m_state->waker.store(ctx.getWaker());

        m_io_service->submit(std::make_unique<WriteRequest>(m_state));
        return PollPending;
    }

    // Check if the operation completed
    if (!m_state->complete.load(std::memory_order_acquire)) {
        // Still pending — update waker
        m_state->waker.store(ctx.getWaker());
        return PollPending;
    }

    // Check for cancellation
    if (m_state->cancelled.load(std::memory_order_acquire)) {
        throw std::runtime_error("Write operation cancelled");
    }

    // Operation completed — check result
    ssize_t result = m_state->result;
    if (result < 0) {
        // Write failed
        throw_uv_error(static_cast<int>(result), "uv_fs_write");
    }

    // Success — return bytes written
    return static_cast<std::size_t>(result);
}

// ---------------------------------------------------------------------------
// File
// ---------------------------------------------------------------------------

File::File(uv_file fd, IoService* io_service)
    : m_fd(fd), m_io_service(io_service) {}

File::File(File&& other) noexcept
    : m_fd(other.m_fd), m_io_service(other.m_io_service) {
    other.m_fd = -1;  // Transfer ownership
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        // Close the current file if open
        if (m_fd >= 0 && m_io_service) {
            m_io_service->submit(std::make_unique<CloseRequest>(m_fd));
        }

        // Transfer ownership
        m_fd = other.m_fd;
        m_io_service = other.m_io_service;
        other.m_fd = -1;
    }
    return *this;
}

File::~File() {
    if (m_fd >= 0 && m_io_service) {
        // Submit async close request
        m_io_service->submit(std::make_unique<CloseRequest>(m_fd));
    }
}

File::OpenFuture File::open(std::string path, FileMode mode) {
    return OpenFuture(std::move(path), mode, &current_io_service());
}

File::ReadFuture File::read(std::span<std::byte> buf) {
    return read_at(buf, -1);  // -1 = use current position
}

File::ReadFuture File::read_at(std::span<std::byte> buf, int64_t offset) {
    return ReadFuture(m_fd, buf, offset, m_io_service);
}

File::WriteFuture File::write(std::span<const std::byte> data) {
    return write_at(data, -1);  // -1 = use current position
}

File::WriteFuture File::write_at(std::span<const std::byte> data, int64_t offset) {
    return WriteFuture(m_fd, data, offset, m_io_service);
}

} // namespace coro
