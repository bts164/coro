#pragma once

#include <coro/detail/waker.h>
#include <uv.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace coro {

// ---------------------------------------------------------------------------
// TimerState
// ---------------------------------------------------------------------------

/**
 * @brief Shared state between a @ref SleepFuture and the I/O thread.
 *
 * Heap-allocated and reference-counted so that either the future or the I/O
 * thread can outlive the other without dangling pointers.
 *
 * @warning `handle` must remain the first data member so that `&state->handle`
 *          is the same address as `state` itself — but do NOT rely on a raw
 *          pointer cast because `TimerState` is not standard-layout (it
 *          contains `std::atomic<std::shared_ptr<>>`). Always set
 *          `handle.data = state` explicitly in `StartTimer::execute()` and
 *          recover via `static_cast<TimerState*>(handle->data)`.
 */
struct TimerState : std::enable_shared_from_this<TimerState> {
    uv_timer_t                                  handle;  // must remain first field
    std::atomic<std::shared_ptr<detail::Waker>> waker;   // written by worker, read by I/O thread
    std::atomic<bool>                           fired{false};
};

// ---------------------------------------------------------------------------
// IoRequest — polymorphic command
// ---------------------------------------------------------------------------

/**
 * @brief Abstract base for all requests submitted to @ref IoService.
 *
 * Each subclass encapsulates one libuv operation. @ref IoService::process_queue()
 * calls `execute()` on the I/O thread without knowing the concrete type, so
 * adding new I/O operations (TCP, UDP, file, …) requires only a new subclass.
 */
struct IoRequest {
    virtual ~IoRequest() = default;
    /// Called on the I/O thread with exclusive access to the uv_loop.
    virtual void execute(uv_loop_t* loop) = 0;
};

// ---------------------------------------------------------------------------
// Callbacks (defined in io_service.cpp; used by StartTimer / CancelTimer)
// ---------------------------------------------------------------------------

/// Fired by libuv when a timer deadline expires.
void timer_cb(uv_timer_t* handle);

/// Fired by libuv after uv_close() completes; frees the TimerState.
void close_cb(uv_handle_t* handle);

// ---------------------------------------------------------------------------
// Concrete request types — timer phase
// ---------------------------------------------------------------------------

/**
 * @brief Registers a one-shot timer on the I/O thread.
 *
 * `state` is a raw pointer; its lifetime is guaranteed by either the owning
 * @ref SleepFuture (while alive) or by the @ref CancelTimer that follows it
 * in the FIFO queue (which holds a `shared_ptr<TimerState>`). Do not reorder
 * the queue.
 */
struct StartTimer : IoRequest {
    TimerState*                            state;     // raw; see lifetime note above
    std::chrono::steady_clock::time_point  deadline;

    StartTimer(TimerState* s, std::chrono::steady_clock::time_point d)
        : state(s), deadline(d) {}

    void execute(uv_loop_t* loop) override;
};

/**
 * @brief Cancels and closes a pending timer on the I/O thread.
 *
 * Holds a `shared_ptr<TimerState>` to keep the state alive until the I/O
 * thread processes this request (the @ref SleepFuture destructor may have
 * already released its own reference by this point).
 */
struct CancelTimer : IoRequest {
    std::shared_ptr<TimerState> state;

    explicit CancelTimer(std::shared_ptr<TimerState> s) : state(std::move(s)) {}

    void execute(uv_loop_t* loop) override;
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

private:
    static void io_async_cb(uv_async_t* handle);   // wakes the loop from any thread
    void io_thread_loop();
    void process_queue();   // called from io_async_cb on the I/O thread

    uv_loop_t  m_uv_loop;
    uv_async_t m_async;     // cross-thread doorbell — the only thread-safe libuv primitive

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
