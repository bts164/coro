#include <coro/runtime/io_service.h>
#include <stdexcept>

namespace coro {

namespace {
    thread_local IoService* t_current_io_service = nullptr;
} // namespace

// io_async_cb fires on the I/O thread whenever uv_async_send() is called from a
// worker thread. Multiple sends before the callback fires are coalesced by libuv,
// so process_queue() must drain the whole queue, not just one entry.
void IoService::io_async_cb(uv_async_t* handle) {
    auto* svc = static_cast<IoService*>(handle->data);
    svc->process_queue();

    if (svc->m_stopping.load()) {
        // Closing the async handle removes the last libuv reference; uv_run() returns.
        uv_close(reinterpret_cast<uv_handle_t*>(handle), nullptr);
    }
}

// ---------------------------------------------------------------------------
// timer_cb / close_cb  (file-scope so StartTimer / CancelTimer can reference them)
// ---------------------------------------------------------------------------

void timer_cb(uv_timer_t* handle) {
    // handle->data holds a heap-allocated shared_ptr<TimerState> placed there by
    // StartTimer::execute(). Recover the raw TimerState* through it.
    auto* sp    = static_cast<std::shared_ptr<TimerState>*>(handle->data);
    auto* state = sp->get();
    // Atomically claim the uv_close. If CancelTimer already claimed it (fired==true),
    // this is a no-op — the handle will be closed by CancelTimer::execute().
    if (!state->fired.exchange(true)) {
        state->waker.load()->wake();
        uv_close(reinterpret_cast<uv_handle_t*>(handle), close_cb);
    }
}

void close_cb(uv_handle_t* handle) {
    // Delete the heap-allocated shared_ptr<TimerState> stored in handle->data.
    // This decrements the ref count; TimerState is freed when SleepFuture and
    // CancelTimer have also released their shared_ptrs.
    delete static_cast<std::shared_ptr<TimerState>*>(handle->data);
}

// ---------------------------------------------------------------------------
// StartTimer
// ---------------------------------------------------------------------------

void StartTimer::execute(uv_loop_t* loop) {
    uv_timer_init(loop, &state->handle);
    // Store a heap-allocated shared_ptr<TimerState> in handle->data. This does NOT
    // create a new TimerState — it creates a shared_ptr wrapper (~16 bytes) that shares
    // ownership of the existing TimerState with SleepFuture and CancelTimer. libuv only
    // passes void* through callbacks, so this is the only way to carry a shared_ptr
    // across the uv_close → close_cb boundary. close_cb deletes the wrapper, which
    // decrements the ref count. TimerState is freed when all three owners release.
    state->handle.data = new std::shared_ptr<TimerState>(state->shared_from_this());
    auto ms = std::chrono::ceil<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    uv_timer_start(&state->handle, timer_cb, static_cast<uint64_t>(std::max<int64_t>(0, ms)), 0);
}

// ---------------------------------------------------------------------------
// CancelTimer
// ---------------------------------------------------------------------------

void CancelTimer::execute(uv_loop_t* /*loop*/) {
    // Atomically claim the uv_close. If timer_cb already fired (fired==true),
    // it owns the close — this is a safe no-op.
    if (!state->fired.exchange(true)) {
        uv_timer_stop(&state->handle);
        uv_close(reinterpret_cast<uv_handle_t*>(&state->handle), close_cb);
    }
}

// ---------------------------------------------------------------------------
// IoService
// ---------------------------------------------------------------------------

IoService::IoService() {
    uv_loop_init(&m_uv_loop);
    uv_async_init(&m_uv_loop, &m_async, IoService::io_async_cb);
    m_async.data = this;
    m_io_thread = std::thread([this] { io_thread_loop(); });
}

IoService::~IoService() {
    stop();
}

void IoService::submit(std::unique_ptr<IoRequest> req) {
    {
        std::lock_guard lk(m_io_queue_mutex);
        m_io_queue.push_back(std::move(req));
    }
    uv_async_send(&m_async);
}

void IoService::stop() {
    if (m_stopping.exchange(true))
        return;  // already stopping — idempotent

    uv_async_send(&m_async);  // wake the loop so io_async_cb sees m_stopping

    if (m_io_thread.joinable())
        m_io_thread.join();

    // Flush any handles that are still closing (e.g. in-flight CancelTimers
    // that arrived just before stop()). uv_loop_close returns UV_EBUSY if any
    // handles remain open; run one more iteration to drain close callbacks.
    if (uv_loop_close(&m_uv_loop) == UV_EBUSY) {
        uv_run(&m_uv_loop, UV_RUN_DEFAULT);
        uv_loop_close(&m_uv_loop);
    }
}

void IoService::io_thread_loop() {
    uv_run(&m_uv_loop, UV_RUN_DEFAULT);
    // Returns when all handles are closed (triggered by io_async_cb calling
    // uv_close(&m_async) after seeing m_stopping == true).
}

void IoService::process_queue() {
    std::deque<std::unique_ptr<IoRequest>> local;
    {
        std::lock_guard lk(m_io_queue_mutex);
        std::swap(local, m_io_queue);
    }
    for (auto& req : local)
        req->execute(&m_uv_loop);
}

// ---------------------------------------------------------------------------
// Thread-local access
// ---------------------------------------------------------------------------

void set_current_io_service(IoService* svc) {
    t_current_io_service = svc;
}

IoService& current_io_service() {
    if (!t_current_io_service)
        throw std::runtime_error("coro::current_io_service(): no runtime active on this thread");
    return *t_current_io_service;
}

} // namespace coro
