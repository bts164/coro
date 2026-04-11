#include <coro/runtime/io_service.h>
#include <libwebsockets.h>
#include <stdexcept>

// Forward declaration — protocol_cb is defined in ws_stream.cpp.
// IoService registers it in the shared client lws context so lws knows how to
// dispatch client WebSocket events without IoService needing the full WsStream header.
namespace coro::detail::ws {
    int protocol_cb(lws* wsi, lws_callback_reasons reason,
                    void* user, void* in, std::size_t len);
}

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
        // Destroy the lws context on the I/O thread before closing the async handle.
        // This must happen here, not in stop() after join(), because lws_context_destroy()
        // removes lws's uv handles from the loop — if they remain, uv_run() never returns
        // and join() deadlocks. After this call, the async handle is the only remaining
        // handle, so closing it causes uv_run() to return.
        if (svc->m_lws_ctx) {
            lws_context_destroy(svc->m_lws_ctx);
            svc->m_lws_ctx = nullptr;
        }
        uv_close(reinterpret_cast<uv_handle_t*>(handle), nullptr);
    }
}

IoService::IoService() {
    uv_loop_init(&m_uv_loop);
    uv_async_init(&m_uv_loop, &m_async, IoService::io_async_cb);
    m_async.data = this;
    m_io_thread = std::thread([this] () noexcept{
        this->io_thread_loop();
    });
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

    // m_lws_ctx is destroyed on the I/O thread in io_async_cb before uv_run() returns,
    // so by the time join() completes the lws context is already gone.
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
    lws_set_log_level(0, nullptr);
    //lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO, nullptr);

    // Create the lws context here, on the I/O thread, so it registers its handles
    // on m_uv_loop from the same thread that will drive the loop. The context must
    // be created before uv_run() so lws can add its handles before the first iteration.
    static const lws_protocols protocols[] = {
        { "coro-ws", coro::detail::ws::protocol_cb, 0, 4096, 0, nullptr, 0 },
        { nullptr, nullptr, 0, 0, 0, nullptr, 0 }
    };

    lws_context_creation_info info{};
    info.options        |= LWS_SERVER_OPTION_LIBUV;
    void *loops = &m_uv_loop;
    info.foreign_loops = &loops;
    info.port            = CONTEXT_PORT_NO_LISTEN;  // client-only context
    info.protocols       = protocols;
    m_lws_ctx = lws_create_context(&info);
    // If lws context creation fails, WebSocket operations will fail at connect time.
    // The loop still runs normally for other I/O (timers, TCP).

    // Signal that lws context is ready (or failed to create)
    {
        std::lock_guard lk(m_lws_mutex);
        m_lws_ready = true;
    }
    m_lws_ready_cv.notify_all();

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

lws_context* IoService::lws_ctx() {
    // Wait until the I/O thread has initialized the lws context
    std::unique_lock lk(m_lws_mutex);
    m_lws_ready_cv.wait(lk, [this] { return m_lws_ready; });
    return m_lws_ctx;
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
