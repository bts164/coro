#include <coro/io/tcp_stream.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <stdexcept>
#include <system_error>

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
// ConnectRequest::execute
//
// Runs on the I/O thread. Allocates and initialises the uv_tcp_t, resolves
// the address synchronously (IPv4 literal only — DNS resolution is a TODO),
// and starts the async connect. Stores a heap-allocated shared_ptr<ConnectState>
// in req.data so connect_cb can recover the state via the uv_connect_t*.
// ---------------------------------------------------------------------------
void TcpStream::ConnectRequest::execute(uv_loop_t* loop) {
    uv_tcp_init(loop, &state->tcp->handle);

    struct sockaddr_in addr;
    int r = uv_ip4_addr(host.c_str(), port, &addr);
    if (r != 0) {
        // Synchronous failure — wake the future with an error immediately.
        state->status.store(r, std::memory_order_release);
        if (auto w = state->waker.load())
            w->wake();
        return;
    }

    // Store a heap-allocated shared_ptr in req.data so connect_cb can access
    // the state. connect_cb deletes the wrapper after use.
    state->req.data = new std::shared_ptr<ConnectState>(state);

    r = uv_tcp_connect(&state->req,
                       &state->tcp->handle,
                       reinterpret_cast<const struct sockaddr*>(&addr),
                       connect_cb);
    if (r != 0) {
        delete static_cast<std::shared_ptr<ConnectState>*>(state->req.data);
        state->req.data = nullptr;
        state->status.store(r, std::memory_order_release);
        if (auto w = state->waker.load())
            w->wake();
    }
    // On success uv_tcp_connect returns 0 and connect_cb will fire later.
}

// ---------------------------------------------------------------------------
// ReadRequest::execute
//
// Runs on the I/O thread. Stores a heap-allocated shared_ptr<ReadState> in
// handle.data so alloc_cb and read_cb can find the state, then starts reading.
// ---------------------------------------------------------------------------
void TcpStream::ReadRequest::execute(uv_loop_t* /*loop*/) {
    state->tcp->handle.data = new std::shared_ptr<ReadState>(state);

    int r = uv_read_start(
        reinterpret_cast<uv_stream_t*>(&state->tcp->handle),
        alloc_cb,
        read_cb);
    if (r != 0) {
        delete static_cast<std::shared_ptr<ReadState>*>(state->tcp->handle.data);
        state->tcp->handle.data = nullptr;
        state->error = r;
        state->complete.store(true, std::memory_order_release);
        if (auto w = state->waker.load())
            w->wake();
    }
}

// ---------------------------------------------------------------------------
// WriteRequest::execute
//
// Runs on the I/O thread. Stores a heap-allocated shared_ptr<WriteState> in
// req.data so write_cb can recover it, then issues the write.
// ---------------------------------------------------------------------------
void TcpStream::WriteRequest::execute(uv_loop_t* /*loop*/) {
    state->req.data = new std::shared_ptr<WriteState>(state);

    int r = uv_write(&state->req,
                     reinterpret_cast<uv_stream_t*>(&state->tcp->handle),
                     &state->buf_desc, 1,
                     write_cb);
    if (r != 0) {
        delete static_cast<std::shared_ptr<WriteState>*>(state->req.data);
        state->req.data = nullptr;
        state->error = r;
        state->complete.store(true, std::memory_order_release);
        if (auto w = state->waker.load())
            w->wake();
    }
}

// ---------------------------------------------------------------------------
// CloseRequest::execute
//
// Runs on the I/O thread. Stores a heap-allocated shared_ptr<Handle> in
// handle.data so close_cb can release the last reference.
// ---------------------------------------------------------------------------
void TcpStream::CloseRequest::execute(uv_loop_t* /*loop*/) {
    handle->handle.data = new std::shared_ptr<Handle>(handle);
    uv_close(reinterpret_cast<uv_handle_t*>(&handle->handle), close_cb);
}

// ---------------------------------------------------------------------------
// connect_cb
//
// Runs on the I/O thread when the TCP handshake completes (or fails).
// Stores the status, deletes the req.data wrapper, and wakes the future.
// ---------------------------------------------------------------------------
void TcpStream::connect_cb(uv_connect_t* req, int status) {
    auto* sp    = static_cast<std::shared_ptr<ConnectState>*>(req->data);
    auto  state = *sp;
    delete sp;
    req->data = nullptr;

    // Store status with release so the worker sees nread/error after acquire on complete.
    state->status.store(status, std::memory_order_release);
    if (auto w = state->waker.load())
        w->wake();
}

// ---------------------------------------------------------------------------
// alloc_cb
//
// Runs on the I/O thread. Provides the ReadState's buffer to libuv.
// handle.data points to the heap-allocated shared_ptr<ReadState> wrapper.
// ---------------------------------------------------------------------------
void TcpStream::alloc_cb(uv_handle_t* handle,
                          std::size_t  /*suggested_size*/,
                          uv_buf_t*    buf) {
    auto* sp    = static_cast<std::shared_ptr<ReadState>*>(handle->data);
    auto& state = **sp;
    // Provide the caller's buffer directly — no copy.
    buf->base = reinterpret_cast<char*>(state.buf.data());
    buf->len  = static_cast<decltype(buf->len)>(state.buf.size());
}

// ---------------------------------------------------------------------------
// read_cb
//
// Runs on the I/O thread after data has been read (or on EOF/error).
// Stops reading so the next read() call can start fresh, records the result,
// deletes the handle.data wrapper, and wakes the future.
// ---------------------------------------------------------------------------
void TcpStream::read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* /*buf*/) {
    if (nread == UV_EAGAIN)
        return;  // spurious wake — libuv may call this with no data, ignore

    uv_read_stop(stream);  // one read() call = one read; stop before waking

    auto* handle = reinterpret_cast<uv_handle_t*>(stream);
    auto* sp     = static_cast<std::shared_ptr<ReadState>*>(handle->data);
    auto  state  = *sp;
    delete sp;
    handle->data = nullptr;

    if (nread == UV_EOF) {
        // Clean EOF — caller sees 0 bytes, no error.
        state->nread = 0;
        state->error = 0;
    } else if (nread < 0) {
        // Genuine read error — surface as error code.
        state->nread = 0;
        state->error = static_cast<int>(nread);
    } else {
        state->nread = nread;
        state->error = 0;
    }

    state->complete.store(true, std::memory_order_release);
    if (auto w = state->waker.load())
        w->wake();
}

// ---------------------------------------------------------------------------
// write_cb
//
// Runs on the I/O thread when uv_write() completes. Records the status,
// deletes the req.data wrapper, and wakes the future.
// ---------------------------------------------------------------------------
void TcpStream::write_cb(uv_write_t* req, int status) {
    auto* sp    = static_cast<std::shared_ptr<WriteState>*>(req->data);
    auto  state = *sp;
    delete sp;
    req->data = nullptr;

    state->error = status;
    state->complete.store(true, std::memory_order_release);
    if (auto w = state->waker.load())
        w->wake();
}

// ---------------------------------------------------------------------------
// close_cb
//
// Runs on the I/O thread after uv_close() completes. Deletes the
// shared_ptr<Handle> wrapper stored in handle.data, releasing the Handle.
// ---------------------------------------------------------------------------
void TcpStream::close_cb(uv_handle_t* handle) {
    delete static_cast<std::shared_ptr<Handle>*>(handle->data);
}

// ---------------------------------------------------------------------------
// TcpStream
// ---------------------------------------------------------------------------

TcpStream::TcpStream(std::shared_ptr<Handle> handle, SingleThreadedUvExecutor* uv_exec)
    : m_handle(std::move(handle))
    , m_uv_exec(uv_exec) {}

TcpStream::TcpStream(TcpStream&&) noexcept = default;
TcpStream& TcpStream::operator=(TcpStream&&) noexcept = default;

TcpStream::~TcpStream() {
    if (m_handle)
        m_uv_exec->submit(std::make_unique<CloseRequest>(std::move(m_handle)));
}

TcpStream::ConnectFuture TcpStream::connect(std::string host, uint16_t port) {
    return ConnectFuture(std::move(host), port, &current_uv_executor());
}

TcpStream::ReadFuture TcpStream::read(std::span<std::byte> buf) {
    return ReadFuture(m_handle, buf, m_uv_exec);
}

TcpStream::WriteFuture TcpStream::write(std::span<const std::byte> data) {
    return WriteFuture(m_handle, data, m_uv_exec);
}

// ---------------------------------------------------------------------------
// ConnectFuture
// ---------------------------------------------------------------------------

TcpStream::ConnectFuture::ConnectFuture(std::string host, uint16_t port, SingleThreadedUvExecutor* uv_exec)
    : m_host(std::move(host))
    , m_port(port)
    , m_uv_exec(uv_exec) {}

PollResult<TcpStream> TcpStream::ConnectFuture::poll(detail::Context& ctx) {
    if (!m_state) {
        // First poll: allocate state, submit ConnectRequest to the uv executor.
        m_state = std::make_shared<ConnectState>();
        m_state->tcp = std::make_shared<Handle>();
        m_state->waker.store(ctx.getWaker());
        m_uv_exec->submit(
            std::make_unique<ConnectRequest>(m_state, m_host, m_port));
        return PollPending;
    }

    int status = m_state->status.load(std::memory_order_acquire);
    if (status == INT_MIN) {
        // Not yet complete — update waker in case we were re-polled.
        m_state->waker.store(ctx.getWaker());
        return PollPending;
    }

    if (status != 0)
        throw_uv_error(status, "TcpStream::connect");

    // Connect succeeded — hand ownership of the Handle to a new TcpStream.
    return TcpStream(std::move(m_state->tcp), m_uv_exec);
}

// ---------------------------------------------------------------------------
// ReadFuture
// ---------------------------------------------------------------------------

TcpStream::ReadFuture::ReadFuture(std::shared_ptr<Handle> handle,
                                   std::span<std::byte>   buf,
                                   SingleThreadedUvExecutor* uv_exec)
    : m_uv_exec(uv_exec) {
    m_state = std::make_shared<ReadState>();
    m_state->tcp = std::move(handle);
    m_state->buf = buf;
}

PollResult<std::size_t> TcpStream::ReadFuture::poll(detail::Context& ctx) {
    if (m_state->complete.load(std::memory_order_acquire)) {
        if (m_state->error != 0)
            throw_uv_error(m_state->error, "TcpStream::read");
        return static_cast<std::size_t>(m_state->nread);
    }

    m_state->waker.store(ctx.getWaker());
    if (!m_state->started) {
        m_state->started = true;
        m_uv_exec->submit(std::make_unique<ReadRequest>(m_state));
    }
    return PollPending;
}

// ---------------------------------------------------------------------------
// WriteFuture
// ---------------------------------------------------------------------------

TcpStream::WriteFuture::WriteFuture(std::shared_ptr<Handle>    handle,
                                     std::span<const std::byte> data,
                                     SingleThreadedUvExecutor*  uv_exec)
    : m_uv_exec(uv_exec) {
    m_state = std::make_shared<WriteState>();
    m_state->tcp = std::move(handle);
    // buf_desc points into the caller's data — caller must keep it alive.
    m_state->buf_desc.base = const_cast<char*>(
        reinterpret_cast<const char*>(data.data()));
    m_state->buf_desc.len = static_cast<decltype(m_state->buf_desc.len)>(data.size());
}

PollResult<void> TcpStream::WriteFuture::poll(detail::Context& ctx) {
    if (m_state->complete.load(std::memory_order_acquire)) {
        if (m_state->error != 0)
            throw_uv_error(m_state->error, "TcpStream::write");
        return PollReady;
    }

    m_state->waker.store(ctx.getWaker());
    if (!m_state->started) {
        m_state->started = true;
        m_uv_exec->submit(std::make_unique<WriteRequest>(m_state));
    }
    return PollPending;
}

} // namespace coro
