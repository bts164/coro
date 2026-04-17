#pragma once

#include <coro/runtime/executor.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <uv.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

// Forward declaration — full type in <libwebsockets.h>, included only by translation
// units that use lws directly.
struct lws_context;

namespace coro {

// ---------------------------------------------------------------------------
// IoRequest — polymorphic command submitted to the uv thread.
//
// Each concrete subclass encapsulates one libuv operation and is defined
// privately by the Future that needs it. SingleThreadedUvExecutor::submit()
// calls execute() on the uv thread without knowing the concrete type.
// ---------------------------------------------------------------------------
struct IoRequest {
    virtual ~IoRequest() = default;
    /// Called on the uv thread with exclusive access to the uv_loop.
    virtual void execute(uv_loop_t* loop) = 0;
};

//! Forward declaration to avoid circular dependency with Runtime.
class Runtime;

/**
 * @brief Full-featured @ref Executor that integrates a libuv event loop into its
 * scheduling loop.
 *
 * Runs a dedicated thread that alternates between draining the coroutine task
 * ready queue and calling `uv_run(UV_RUN_ONCE)`:
 *
 * @code
 * loop:
 *   drain_incoming_wakes()      // remote enqueue() calls → m_ready
 *   process_io_queue()          // IoRequest submissions
 *   drain_ready_tasks()         // poll all ready coroutine tasks
 *   uv_run(UV_RUN_ONCE)         // process I/O events; blocks when idle
 * @endcode
 *
 * The `uv_async_t` doorbell wakes a blocking `uv_run()` whenever a remote
 * enqueue() or submit() arrives, or when a uv callback wakes a task that needs
 * re-polling.
 *
 * ### Coroutine tasks
 * Submit tasks via `schedule()` (initial dispatch) or `enqueue()` (re-wakeup via
 * @ref TaskWaker). Tasks polled here receive a @ref TaskWaker whose `executor`
 * pointer targets this executor, so `wake()` routes back through `enqueue()`.
 *
 * ### IoRequest submissions
 * `submit(unique_ptr<IoRequest>)` enqueues a request to be executed on the uv
 * thread. `req->execute(loop)` is called with exclusive access to the uv_loop.
 *
 * ### Thread safety
 * All libuv API calls happen on the uv thread. `enqueue()` and `submit()` are
 * safe to call from any thread.
 */
class SingleThreadedUvExecutor : public Executor {
public:
    SingleThreadedUvExecutor(Runtime *runtime = nullptr);
    ~SingleThreadedUvExecutor() override;

    SingleThreadedUvExecutor(const SingleThreadedUvExecutor&)            = delete;
    SingleThreadedUvExecutor& operator=(const SingleThreadedUvExecutor&) = delete;

    // -----------------------------------------------------------------------
    // Executor interface
    // -----------------------------------------------------------------------

    /// Sets `task->scheduling_state` to `Notified` and calls `enqueue()`.
    void schedule(std::unique_ptr<detail::Task> task) override;

    /// Routes a newly-notified task to the appropriate queue.
    /// - Called from the uv thread: pushes directly to `m_ready`; schedules a
    ///   wakeup via `uv_async_send` so the next loop iteration drains it.
    /// - Called from any other thread: pushes to `m_incoming_wakes` and calls
    ///   `uv_async_send` to wake the uv thread.
    void enqueue(std::shared_ptr<detail::Task> task) override;

    /// Blocks the calling thread until `state.terminated` is true.
    /// The uv thread drives all polling; the caller just waits on `state.cv`.
    void wait_for_completion(detail::TaskStateBase& state) override;

    /// Submits an IoRequest to be executed on the uv thread.
    void submit(std::unique_ptr<IoRequest> req);

    /// Signals the uv thread to stop and joins it. Idempotent.
    void stop();

    /// Returns the libwebsockets context. Blocks until the uv thread has
    /// initialized it. Thread-safe.
    lws_context* lws_ctx();

    /// Returns the underlying uv_loop_t*. Must only be called from the uv thread
    /// (or during initialisation) — libuv is not thread-safe.
    uv_loop_t* loop() noexcept { return &m_uv_loop; }

private:
    static void io_async_cb(uv_async_t* handle);
    void io_thread_loop();

    /// Moves all tasks from m_incoming_wakes into m_ready.
    /// Called on the uv thread only.
    void drain_incoming_wakes();

    /// Drains and executes the IoRequest queue on the uv thread.
    void process_io_queue();

    /// Polls all tasks currently in m_ready through the normal CAS state machine.
    /// Called on the uv thread only (inside io_async_cb).
    void drain_ready_tasks();

    // -----------------------------------------------------------------------
    // libuv infrastructure
    // -----------------------------------------------------------------------
    uv_loop_t    m_uv_loop;
    lws_context* m_lws_ctx = nullptr;
    uv_async_t   m_async;              // cross-thread doorbell — the only thread-safe uv primitive

    // lws context initialization — uv thread signals ready after creating m_lws_ctx
    std::mutex              m_lws_mutex;
    std::condition_variable m_lws_ready_cv;
    bool                    m_lws_ready = false;

    // -----------------------------------------------------------------------
    // Coroutine task queues
    // -----------------------------------------------------------------------

    // Local ready queue — accessed only from the uv thread; no lock needed.
    std::queue<std::shared_ptr<detail::Task>> m_ready;

    // Remote injection queue — written from any thread, drained on uv thread.
    std::deque<std::shared_ptr<detail::Task>> m_incoming_wakes;
    std::mutex                                m_remote_mutex;

    // -----------------------------------------------------------------------
    // IoRequest queue
    // -----------------------------------------------------------------------
    std::mutex                             m_io_queue_mutex;
    std::deque<std::unique_ptr<IoRequest>> m_io_queue;

    // -----------------------------------------------------------------------
    // Thread management
    // -----------------------------------------------------------------------
    std::thread::id   m_uv_thread_id;  // set at thread start; used by enqueue()
    std::thread       m_uv_thread;
    std::atomic<bool> m_stopping{false};
};

// ---------------------------------------------------------------------------
// Thread-local access
// ---------------------------------------------------------------------------

/// Sets the thread-local SingleThreadedUvExecutor pointer. Called by Runtime
/// on both the calling thread and the uv thread at startup.
void set_current_uv_executor(SingleThreadedUvExecutor* exec);

/// Returns the thread-local SingleThreadedUvExecutor.
/// @throws std::runtime_error if called outside a Runtime context.
SingleThreadedUvExecutor& current_uv_executor();

} // namespace coro
