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

    // Flush any handles that are still closing (e.g. in-flight CancelRequests
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
