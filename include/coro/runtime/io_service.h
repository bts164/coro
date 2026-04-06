#pragma once

#include <uv.h>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>

// Forward declaration — full type defined by <libwebsockets.h>, included only by
// translation units that use lws directly. IoService holds the context by pointer
// so this header does not need to pull in the lws headers.
struct lws_context;

namespace coro {

// ---------------------------------------------------------------------------
// IoRequest — polymorphic command
// ---------------------------------------------------------------------------

/**
 * @brief Abstract base for all requests submitted to @ref IoService.
 *
 * Each concrete subclass encapsulates one libuv operation and is defined
 * privately by the Future that needs it — @ref IoService knows nothing about
 * specific operation types. @ref IoService::process_queue() calls `execute()`
 * on the I/O thread without knowing the concrete type, so adding new I/O
 * operations (TCP, UDP, file, …) requires only a new subclass, defined
 * alongside the Future that uses it.
 */
struct IoRequest {
    virtual ~IoRequest() = default;
    /// Called on the I/O thread with exclusive access to the uv_loop.
    virtual void execute(uv_loop_t* loop) = 0;
};

// ---------------------------------------------------------------------------
// IoService
// ---------------------------------------------------------------------------

/**
 * @brief Owns the libuv event loop and its dedicated I/O thread.
 *
 * Worker threads submit @ref IoRequest objects via @ref submit(). The I/O
 * thread drains the queue whenever `uv_async_send` wakes it, then runs
 * `uv_run` until the next event or shutdown signal.
 *
 * `IoService` must be declared **before** the @ref Executor in @ref Runtime
 * so that it is destroyed *after* all executor worker threads have joined.
 * This guarantees that `waker->wake()` is never called after the loop closes.
 *
 * `IoService` has no knowledge of any specific I/O operation type. All
 * operation-specific state and callbacks live in the Future that owns them.
 */
class IoService {
public:
    IoService();
    ~IoService();

    IoService(const IoService&)            = delete;
    IoService& operator=(const IoService&) = delete;

    /// Thread-safe. Enqueues `req` and signals the I/O thread.
    void submit(std::unique_ptr<IoRequest> req);

    /// Signals the I/O thread to stop and joins it. Idempotent.
    void stop();

    /// Returns the libwebsockets context owned by this IoService.
    /// Non-null only after the I/O thread has started. Must only be used
    /// from the I/O thread or before the first submit() call.
    lws_context* lws_ctx() const noexcept { return m_lws_ctx; }

private:
    static void io_async_cb(uv_async_t* handle);   // wakes the loop from any thread
    void io_thread_loop();
    void process_queue();   // called from io_async_cb on the I/O thread

    uv_loop_t    m_uv_loop;
    lws_context* m_lws_ctx = nullptr;  // created at start of io_thread_loop(), destroyed in stop()
    uv_async_t   m_async;   // cross-thread doorbell — the only thread-safe libuv primitive

    std::thread m_io_thread;

    std::mutex                             m_io_queue_mutex;
    std::deque<std::unique_ptr<IoRequest>> m_io_queue;
    std::atomic<bool>                      m_stopping{false};
};

// ---------------------------------------------------------------------------
// Thread-local access
// ---------------------------------------------------------------------------

/// Sets the thread-local IoService pointer. Called by Runtime::block_on() and
/// every executor worker thread at startup.
void set_current_io_service(IoService* svc);

/// Returns the thread-local IoService.
/// @throws std::runtime_error if called outside a Runtime::block_on() context.
IoService& current_io_service();

} // namespace coro
