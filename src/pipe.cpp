#include <coro/io/pipe.h>
#include <coro/coro.h>
#include <coro/runtime/uv_future.h>
#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/task/spawn_on.h>
#include <atomic>
#include <fcntl.h>
#include <queue>
#include <sys/stat.h>
#include <system_error>
#include <vector>

namespace coro {

// ---------------------------------------------------------------------------
// PipeState — shared between Pipe, write driver, read driver, and handles.
// ---------------------------------------------------------------------------

namespace detail {

struct PipeState {
    uv_pipe_t              handle;
    std::mutex             mutex;

    std::queue<std::shared_ptr<WriteRequest>> write_queue;         // GUARDED BY mutex
    std::shared_ptr<Waker>                    write_driver_waker;  // GUARDED BY mutex

    std::queue<std::shared_ptr<ReadRequest>>  read_queue;          // GUARDED BY mutex
    std::shared_ptr<Waker>                    read_driver_waker;   // GUARDED BY mutex
    // Set by read_driver before uv_read_start, cleared after. read_cb signals it when
    // the read queue empties; Pipe destructor may signal it early to interrupt the driver.
    UvCallbackResult<>*                       read_done_signal = nullptr;  // GUARDED BY mutex

    bool closing = false;  // GUARDED BY mutex; set once to true, never reset

    // Counts running drivers (starts at 2). The last driver to exit calls uv_close.
    std::atomic<int> active_drivers{2};

    SingleThreadedUvExecutor* exec = nullptr;
};

} // namespace detail

// ---------------------------------------------------------------------------
// WriteHandle::poll
// ---------------------------------------------------------------------------

PollResult<void> WriteHandle::poll(detail::Context& ctx) {
    std::lock_guard lock(m_req->mutex);
    if (!m_req->completed) {
        m_req->waker = ctx.getWaker();
        return PollPending;
    }
    if (m_req->error)
        return PollError(std::make_exception_ptr(
            std::system_error(m_req->error, "Pipe::write")));
    return PollReady;
}

// ---------------------------------------------------------------------------
// push helpers — called from Pipe::write / Pipe::read (pipe.hpp)
// ---------------------------------------------------------------------------

void detail::push_write_request(detail::PipeState& state,
                                 std::shared_ptr<detail::WriteRequest> req) {
    std::shared_ptr<detail::Waker> waker;
    {
        std::lock_guard lock(state.mutex);
        state.write_queue.push(req);
        waker = std::move(state.write_driver_waker);
    }
    if (waker) waker->wake();
}

void detail::push_read_request(detail::PipeState& state,
                                std::shared_ptr<detail::ReadRequest> req) {
    std::shared_ptr<detail::Waker> waker;
    {
        std::lock_guard lock(state.mutex);
        state.read_queue.push(req);
        waker = std::move(state.read_driver_waker);
    }
    if (waker) waker->wake();
}

// ---------------------------------------------------------------------------
// Internal helpers (anonymous namespace)
// ---------------------------------------------------------------------------

namespace {

[[noreturn]] void throw_uv_error(int status, const char* what) {
    throw std::system_error(std::error_code(-status, std::system_category()), what);
}

int translate_flags(PipeMode mode) {
    switch (mode) {
        case PipeMode::Read:      return O_RDONLY;
        case PipeMode::Write:     return O_WRONLY;
        case PipeMode::ReadWrite: return O_RDWR;
    }
    return O_RDONLY;
}

// Signal drivers to stop — extracted so both ~Pipe() and operator=(Pipe&&) can call it.
void signal_close(detail::PipeState& state) {
    std::shared_ptr<detail::Waker> write_waker, read_waker;
    detail::UvCallbackResult<>* done_signal = nullptr;
    {
        std::lock_guard lock(state.mutex);
        state.closing  = true;
        write_waker    = std::move(state.write_driver_waker);
        read_waker     = std::move(state.read_driver_waker);
        done_signal    = state.read_done_signal;
    }
    // Interrupt the read driver if it is suspended in co_await wait(stopped).
    if (done_signal) done_signal->complete();
    if (write_waker) write_waker->wake();
    if (read_waker)  read_waker->wake();
}

// Drain queued write requests with a broken-pipe error.
void drain_write_queue(detail::PipeState& state) {
    std::queue<std::shared_ptr<detail::WriteRequest>> q;
    {
        std::lock_guard lock(state.mutex);
        q = std::move(state.write_queue);
    }
    auto ec = std::make_error_code(std::errc::broken_pipe);
    while (!q.empty()) {
        auto req = std::move(q.front()); q.pop();
        std::shared_ptr<detail::Waker> waker;
        {
            std::lock_guard lock(req->mutex);
            req->completed = true;
            req->error     = ec;
            waker = std::move(req->waker);
        }
        if (waker) waker->wake();
    }
}

// Drain queued read requests with a broken-pipe error.
void drain_read_queue(detail::PipeState& state) {
    std::queue<std::shared_ptr<detail::ReadRequest>> q;
    {
        std::lock_guard lock(state.mutex);
        q = std::move(state.read_queue);
    }
    auto ec = std::make_error_code(std::errc::broken_pipe);
    while (!q.empty()) {
        auto req = std::move(q.front()); q.pop();
        std::shared_ptr<detail::Waker> waker;
        {
            std::lock_guard lock(req->mutex);
            req->completed = true;
            req->error     = ec;
            waker = std::move(req->waker);
        }
        if (waker) waker->wake();
    }
}

// ---------------------------------------------------------------------------
// Driver park futures
// ---------------------------------------------------------------------------

// WriteDriverPark: parks until write_queue is non-empty or closing.
struct WriteDriverPark {
    using OutputType = void;
    std::shared_ptr<detail::PipeState> state;

    PollResult<void> poll(detail::Context& ctx) {
        std::lock_guard lock(state->mutex);
        if (!state->write_queue.empty() || state->closing)
            return PollReady;
        state->write_driver_waker = ctx.getWaker();
        return PollPending;
    }
};

// ReadDriverPark: parks until read_queue is non-empty or closing.
struct ReadDriverPark {
    using OutputType = void;
    std::shared_ptr<detail::PipeState> state;

    PollResult<void> poll(detail::Context& ctx) {
        std::lock_guard lock(state->mutex);
        if (!state->read_queue.empty() || state->closing)
            return PollReady;
        state->read_driver_waker = ctx.getWaker();
        return PollPending;
    }
};

// ---------------------------------------------------------------------------
// libuv read callbacks
// ---------------------------------------------------------------------------

// alloc_cb: get a buffer for the next read from the front of read_queue.
// Returns zero-length if the queue is empty, triggering UV_ENOBUFS in read_cb.
void pipe_alloc_cb(uv_handle_t* handle, size_t /*suggested*/, uv_buf_t* buf) {
    auto* state = static_cast<detail::PipeState*>(handle->data);
    std::lock_guard lock(state->mutex);
    if (state->read_queue.empty()) {
        *buf = uv_buf_init(nullptr, 0);
        return;
    }
    auto& req = state->read_queue.front();
    *buf = uv_buf_init(req->base, static_cast<unsigned>(req->capacity));
}

// read_cb: handle incoming data or errors from libuv.
void pipe_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* /*buf*/) {
    auto* state = static_cast<detail::PipeState*>(stream->data);

    std::shared_ptr<detail::ReadRequest> req;
    bool should_stop = false;
    detail::UvCallbackResult<>* done_signal = nullptr;

    {
        std::lock_guard lock(state->mutex);

        if (nread == UV_ENOBUFS || state->read_queue.empty()) {
            // Queue empty when alloc_cb ran — stop reading.
            should_stop = true;
            done_signal = state->read_done_signal;
        } else {
            req = state->read_queue.front();
            state->read_queue.pop();
            should_stop = state->read_queue.empty() || nread <= 0;
            if (should_stop)
                done_signal = state->read_done_signal;
        }
    }

    if (should_stop)
        uv_read_stop(stream);

    if (req) {
        std::error_code ec;
        std::size_t filled = 0;
        if (nread < 0)
            ec = std::error_code(static_cast<int>(-nread), std::system_category());
        else
            filled = static_cast<std::size_t>(nread);

        std::shared_ptr<detail::Waker> waker;
        {
            std::lock_guard req_lock(req->mutex);
            req->filled    = filled;
            req->completed = true;
            req->error     = ec;
            waker = std::move(req->waker);
        }
        if (waker) waker->wake();
    }

    if (done_signal)
        done_signal->complete();
}

// ---------------------------------------------------------------------------
// Write driver — runs on uv thread, batches queued writes into one uv_write.
// ---------------------------------------------------------------------------

coro::Coro<void> write_driver(std::shared_ptr<detail::PipeState> state) {
    while (true) {
        co_await WriteDriverPark{state};

        {
            std::lock_guard lock(state->mutex);
            if (state->closing) break;
        }

        // Drain the entire write queue into a local batch.
        std::vector<std::shared_ptr<detail::WriteRequest>> batch;
        {
            std::lock_guard lock(state->mutex);
            while (!state->write_queue.empty()) {
                batch.push_back(std::move(state->write_queue.front()));
                state->write_queue.pop();
            }
        }

        if (batch.empty()) continue;

        // Build one uv_buf_t per request and issue a single uv_write.
        // bufs, write_req, and cb_result live on the coroutine frame and remain
        // valid while suspended in co_await wait(cb_result).
        std::vector<uv_buf_t> bufs;
        bufs.reserve(batch.size());
        for (auto& req : batch)
            bufs.push_back(uv_buf_init(const_cast<char*>(req->base),
                                       static_cast<unsigned>(req->size)));

        uv_write_t            write_req;
        UvCallbackResult<int> cb_result;
        write_req.data = &cb_result;

        int r = uv_write(&write_req,
                         reinterpret_cast<uv_stream_t*>(&state->handle),
                         bufs.data(), static_cast<unsigned>(bufs.size()),
                         [](uv_write_t* req, int status) {
                             static_cast<UvCallbackResult<int>*>(req->data)
                                 ->complete(status);
                         });

        std::error_code ec;
        if (r < 0) {
            ec = std::error_code(-r, std::system_category());
        } else {
            auto [status] = co_await wait(cb_result);
            if (status < 0)
                ec = std::error_code(-status, std::system_category());
        }

        for (auto& req : batch) {
            std::shared_ptr<detail::Waker> waker;
            {
                std::lock_guard lock(req->mutex);
                req->completed = true;
                req->error     = ec;
                waker = std::move(req->waker);
            }
            if (waker) waker->wake();
        }
    }

    drain_write_queue(*state);

    // Last driver to exit closes the uv_pipe_t handle.
    if (state->active_drivers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        UvCallbackResult<> close_result;
        state->handle.data = &close_result;
        uv_close(reinterpret_cast<uv_handle_t*>(&state->handle),
                 [](uv_handle_t* h) {
                     static_cast<UvCallbackResult<>*>(h->data)->complete();
                 });
        co_await wait(close_result);
    }
}

// ---------------------------------------------------------------------------
// Read driver — runs on uv thread, fills queued read requests via uv_read_start.
// ---------------------------------------------------------------------------

coro::Coro<void> read_driver(std::shared_ptr<detail::PipeState> state) {
    // Store state pointer in handle.data for alloc_cb / read_cb.
    // Safe: state is alive (shared_ptr) for the driver's entire lifetime.
    state->handle.data = state.get();

    while (true) {
        co_await ReadDriverPark{state};

        {
            std::lock_guard lock(state->mutex);
            if (state->closing) break;
        }

        // Set read_done_signal under lock; re-check closing in case the destructor
        // raced with the ReadDriverPark return.
        UvCallbackResult<> stopped;
        bool already_closing = false;
        {
            std::lock_guard lock(state->mutex);
            if (state->closing)
                already_closing = true;
            else
                state->read_done_signal = &stopped;
        }
        if (already_closing) break;

        // Start event-driven reading. alloc_cb / read_cb deliver data and signal
        // `stopped` when the queue empties. The destructor may also signal `stopped`
        // early; UvCallbackResult::complete() is safe to call multiple times.
        uv_read_start(reinterpret_cast<uv_stream_t*>(&state->handle),
                      pipe_alloc_cb, pipe_read_cb);

        co_await wait(stopped);

        bool stop_reading;
        {
            std::lock_guard lock(state->mutex);
            state->read_done_signal = nullptr;
            stop_reading = state->closing;
        }
        if (stop_reading) {
            // uv_read_stop may have already been called by read_cb; calling it again
            // is a safe no-op.
            uv_read_stop(reinterpret_cast<uv_stream_t*>(&state->handle));
            break;
        }
    }

    drain_read_queue(*state);

    // Last driver to exit closes the uv_pipe_t handle.
    if (state->active_drivers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        UvCallbackResult<> close_result;
        state->handle.data = &close_result;
        uv_close(reinterpret_cast<uv_handle_t*>(&state->handle),
                 [](uv_handle_t* h) {
                     static_cast<UvCallbackResult<>*>(h->data)->complete();
                 });
        co_await wait(close_result);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Pipe — construction / move / destruction
// ---------------------------------------------------------------------------

Pipe::Pipe(std::shared_ptr<detail::PipeState> state)
    : m_state(std::move(state)) {}

Pipe::Pipe(Pipe&& other) noexcept
    : m_state(std::move(other.m_state)) {}

Pipe& Pipe::operator=(Pipe&& other) noexcept {
    if (this != &other) {
        if (m_state) signal_close(*m_state);
        m_state = std::move(other.m_state);
    }
    return *this;
}

Pipe::~Pipe() {
    if (!m_state) return;
    signal_close(*m_state);
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

Pipe::OpenFuture Pipe::open(std::string path, PipeMode mode) {
    int flags  = translate_flags(mode);
    auto& exec = current_uv_executor();
    return with_context(exec,
        [](SingleThreadedUvExecutor& exec,
           std::string path, int flags) -> Coro<Pipe> {

            // uv_fs_open blocks on the thread pool until both FIFO ends are open.
            UvCallbackResult<uv_file> open_result;
            uv_fs_t open_req;
            open_req.data = &open_result;

            int r = uv_fs_open(exec.loop(), &open_req, path.c_str(), flags, 0,
                [](uv_fs_t* r) {
                    static_cast<UvCallbackResult<uv_file>*>(r->data)
                        ->complete(static_cast<uv_file>(r->result));
                    uv_fs_req_cleanup(r);
                });
            if (r < 0) throw_uv_error(r, "Pipe::open");

            auto [fd] = co_await wait(open_result);
            if (fd < 0) throw_uv_error(static_cast<int>(fd), "Pipe::open");

            // Wrap the fd in a uv_pipe_t for event-driven stream I/O.
            auto state  = std::make_shared<detail::PipeState>();
            state->exec = &exec;

            if (int rv = uv_pipe_init(exec.loop(), &state->handle, /*ipc=*/0); rv < 0)
                throw_uv_error(rv, "uv_pipe_init");

            if (int rv = uv_pipe_open(&state->handle,
                                       static_cast<uv_os_fd_t>(fd)); rv < 0)
                throw_uv_error(rv, "uv_pipe_open");

            // Spawn background drivers; they run for the lifetime of the Pipe.
            spawn_on(exec, write_driver(state)).detach();
            spawn_on(exec, read_driver(state)).detach();

            co_return Pipe(std::move(state));
        }(exec, std::move(path), flags)
    );
}

// ---------------------------------------------------------------------------
// create
// ---------------------------------------------------------------------------

Pipe::CreateFuture Pipe::create(std::string path, int permission) {
    auto& exec = current_uv_executor();
    return with_context(exec,
        [](SingleThreadedUvExecutor& exec,
           std::string path, int permission) -> Coro<void> {

            // mkfifo is a quick metadata op but may block on slow filesystems;
            // offload it to the libuv thread pool.
            struct WorkCtx {
                std::string            path;
                int                    permission;
                int                    rc;
                UvCallbackResult<int>* result;
            };

            UvCallbackResult<int> result;
            uv_work_t req;
            auto* ctx = new WorkCtx{std::move(path), permission, 0, &result};
            req.data  = ctx;

            int r = uv_queue_work(exec.loop(), &req,
                [](uv_work_t* w) {
                    auto* c = static_cast<WorkCtx*>(w->data);
                    if (::mkfifo(c->path.c_str(),
                                 static_cast<mode_t>(c->permission)) != 0)
                        c->rc = -errno;
                },
                [](uv_work_t* w, int /*status*/) {
                    auto* c = static_cast<WorkCtx*>(w->data);
                    c->result->complete(c->rc);
                    delete c;
                });

            if (r < 0) throw_uv_error(r, "Pipe::create");

            auto [rc] = co_await wait(result);
            if (rc != 0 && rc != -EEXIST)
                throw std::system_error(
                    std::error_code(-rc, std::system_category()), "Pipe::create");
        }(exec, std::move(path), permission)
    );
}

} // namespace coro
