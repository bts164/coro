#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/detail/context.h>
#include <libwebsockets.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

// Forward declaration — protocol_cb is defined in ws_stream.cpp.
namespace coro::detail::ws {
    int protocol_cb(lws* wsi, lws_callback_reasons reason,
                    void* user, void* in, std::size_t len);
}

namespace coro {

namespace {
    thread_local SingleThreadedUvExecutor* t_current_uv_executor = nullptr;
} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SingleThreadedUvExecutor::SingleThreadedUvExecutor(Runtime * /*runtime*/) {
    uv_loop_init(&m_uv_loop);
    uv_async_init(&m_uv_loop, &m_async, SingleThreadedUvExecutor::io_async_cb);
    m_async.data = this;
    m_uv_thread = std::thread([this]() noexcept {
        this->io_thread_loop();
    });
    // get_id() is available as soon as the std::thread object exists (the OS
    // assigns the id at pthread_create) — no need to wait for io_thread_loop
    // to actually start running. Setting it here, on the constructing thread,
    // avoids a data race with enqueue() being called (from another thread)
    // before io_thread_loop got a chance to set it itself.
    m_uv_thread_id = m_uv_thread.get_id();
}

SingleThreadedUvExecutor::~SingleThreadedUvExecutor() {
    stop();
}

// ---------------------------------------------------------------------------
// Executor interface
// ---------------------------------------------------------------------------

void SingleThreadedUvExecutor::schedule(std::shared_ptr<detail::TaskBase> task) {
    task->owning_executor = this;
    task->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_owned_mutex);
        m_owned_tasks.insert(task);
    }
    enqueue(std::move(task));
}

void SingleThreadedUvExecutor::enqueue(std::shared_ptr<detail::TaskBase> task) {
    if (std::this_thread::get_id() == m_uv_thread_id) {
        // On the uv thread — push directly to the local ready queue.
        // uv_async_send schedules another io_async_cb to drain it after the
        // current one (or the next uv_run iteration) completes.
        m_ready.push(std::move(task));
        uv_async_send(&m_async);
    } else {
        // Remote thread — hand off via injection queue and wake the uv thread.
        {
            std::lock_guard lock(m_remote_mutex);
            m_incoming_wakes.push_back(std::move(task));
        }
        uv_async_send(&m_async);
    }
}

void SingleThreadedUvExecutor::wait_for_completion(detail::TaskStateBase& state) {
    // The uv thread drives all polling. The calling thread just waits.
    state.wait_until_done();
}

void SingleThreadedUvExecutor::stop() {
    if (m_stopping.exchange(true))
        return;  // idempotent

    uv_async_send(&m_async);  // wake so io_async_cb sees m_stopping

    if (m_uv_thread.joinable())
        m_uv_thread.join();

    // ---------------------------------------------------------------------------
    // Phase 2 of lws + libuv shutdown (see io_async_cb for phase 1).
    //
    // The UV thread has exited — uv_run() returned because io_async_cb called
    // uv_stop(). At this point lws has begun its teardown (phase 1 called
    // lws_context_destroy once) but its async close callbacks may not have fired
    // yet because uv_stop() cuts the loop short before they get a chance to run.
    //
    // lws_context_destroy must be called a second time from outside the loop to
    // complete whatever teardown was interrupted. This is the documented usage
    // for foreign libuv loops: the first call (inside the loop) schedules
    // cleanup; the second call (after the loop exits) finalises it. lws is
    // internally idempotent across these two calls — it checks its own state and
    // continues from where it left off rather than double-freeing.
    //
    // After the second lws_context_destroy we walk and force-close any remaining
    // handles. In normal operation this catches:
    //   - m_async (our wakeup handle, still open since we used uv_stop not uv_close)
    //   - any handles owned by user code that were not closed before the runtime
    //     shut down (e.g. a TcpStream that went out of scope without co_await close)
    //
    // A final uv_run(UV_RUN_DEFAULT) drains the close callbacks for all of those
    // handles (including any remaining lws close callbacks), after which
    // uv_loop_close() should succeed.
    // ---------------------------------------------------------------------------

    if (m_lws_ctx) {
        // Second call — finalises the teardown that was started in io_async_cb.
        // uv_stop() cut the loop short before lws's async close callbacks could
        // all fire; this second call completes whatever was left. lws is
        // internally idempotent: it checks its own destruction state and resumes
        // from where it left off rather than double-freeing anything.
        lws_context_destroy(m_lws_ctx);
        m_lws_ctx = nullptr;
    }

    uv_walk(&m_uv_loop, [](uv_handle_t* h, void*) {
        if (!uv_is_closing(h))
            uv_close(h, nullptr);
    }, nullptr);

    uv_run(&m_uv_loop, UV_RUN_DEFAULT);
    uv_loop_close(&m_uv_loop);
}

lws_context* SingleThreadedUvExecutor::lws_ctx() {
    std::unique_lock lk(m_lws_mutex);
    m_lws_ready_cv.wait(lk, [this] { return m_lws_ready; });
    return m_lws_ctx;
}

// ---------------------------------------------------------------------------
// uv thread
// ---------------------------------------------------------------------------

void SingleThreadedUvExecutor::io_async_cb(uv_async_t* handle) {
    auto* self = static_cast<SingleThreadedUvExecutor*>(handle->data);

    self->drain_incoming_wakes();
    self->drain_ready_tasks();

    if (self->m_stopping.load()) {
        // ---------------------------------------------------------------------------
        // Phase 1 of lws + libuv shutdown.
        //
        // lws shutdown with a foreign libuv loop is a two-phase process and is
        // poorly documented. Here is the full picture, derived from reading the lws
        // source (lib/event-libs/libuv/libuv.c) and its minimal foreign-loop example
        // (minimal-examples/http-server/minimal-http-server-eventlib-foreign/libuv.c):
        //
        // PHASE 1 — called from within the running loop (here, from io_async_cb):
        //
        //   lws_context_destroy() starts lws teardown:
        //     - Calls uv_poll_stop() + uv_close() on every wsi's poll handle.
        //     - Calls uv_idle_stop() + uv_close() and uv_timer_stop() + uv_close()
        //       on its per-thread static handles (idle, sultimer).
        //     - All of these uv_close() calls are synchronous in marking handles as
        //       "closing" (uv_is_closing() returns true immediately), but their close
        //       CALLBACKS fire asynchronously in later loop iterations.
        //     - For a foreign loop, lws does NOT call uv_stop() — that is our job.
        //     - For a foreign loop, lws does NOT free context memory yet; it defers
        //       to a second lws_context_destroy() call after the loop exits.
        //
        //   uv_stop() tells uv_run() to return after the current iteration. We use
        //   this instead of closing our handles (e.g. m_async) to unblock uv_run,
        //   because closing handles here races with lws's own deferred close
        //   callbacks. uv_stop() is clean: no handles are touched, the loop just
        //   exits at the next safe point.
        //
        // PHASE 2 — after uv_run() returns (see stop()):
        //   - lws_context_destroy() is called a second time to finalise cleanup that
        //     was interrupted when uv_stop() cut the loop short.
        //   - uv_walk() + uv_close() closes all remaining handles (m_async plus any
        //     user-leaked handles).
        //   - A final uv_run(UV_RUN_DEFAULT) drains all close callbacks.
        //   - uv_loop_close() completes the teardown.
        // ---------------------------------------------------------------------------
        if (self->m_lws_ctx) {
            // First call — starts teardown and marks all lws handles as closing.
            // Do NOT clear m_lws_ctx here; stop() needs the pointer for the
            // mandatory second call after uv_run() returns.
            lws_context_destroy(self->m_lws_ctx);
        }
        uv_stop(&self->m_uv_loop);
        return;
    }

    // If tasks were woken during drain_ready_tasks (pushed to m_ready via
    // enqueue() on the uv thread), schedule another callback to drain them.
    if (!self->m_ready.empty()) {
        uv_async_send(handle);
    }
}

void SingleThreadedUvExecutor::io_thread_loop() {
    set_current_uv_executor(this);

    lws_set_log_level(0, nullptr);

    // Create the lws context on the uv thread so it registers its handles on
    // m_uv_loop from the thread that will drive it.
    static const lws_protocols protocols[] = {
        { "coro-ws", coro::detail::ws::protocol_cb, 0, 4096, 0, nullptr, 0 },
        { nullptr, nullptr, 0, 0, 0, nullptr, 0 }
    };

    lws_context_creation_info info{};
    info.options        |= LWS_SERVER_OPTION_LIBUV;
    void* loops          = &m_uv_loop;
    info.foreign_loops   = &loops;
    info.port            = CONTEXT_PORT_NO_LISTEN;
    info.protocols       = protocols;
    m_lws_ctx = lws_create_context(&info);

    {
        std::lock_guard lk(m_lws_mutex);
        m_lws_ready = true;
    }
    m_lws_ready_cv.notify_all();

    uv_run(&m_uv_loop, UV_RUN_DEFAULT);
}

void SingleThreadedUvExecutor::drain_incoming_wakes() {
    std::deque<std::shared_ptr<detail::TaskBase>> local;
    {
        std::lock_guard lock(m_remote_mutex);
        std::swap(local, m_incoming_wakes);
    }
    for (auto& t : local)
        m_ready.push(std::move(t));
}

void SingleThreadedUvExecutor::drain_ready_tasks() {
    // Snapshot count — tasks enqueued during this pass are deferred to the
    // next io_async_cb firing so we don't loop indefinitely.
    const auto count = m_ready.size();
    for (std::size_t i = 0; i < count && !m_ready.empty(); ++i) {
        auto task = std::move(m_ready.front());
        m_ready.pop();

        auto expected = detail::SchedulingState::Notified;
        if (!task->scheduling_state.compare_exchange_strong(
                expected, detail::SchedulingState::Running,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            std::cerr << "[coro] SingleThreadedUvExecutor: unexpected scheduling_state "
                      << static_cast<int>(expected)
                      << " during Notified→Running transition\n";
            std::abort();
        }

        detail::Context ctx(std::static_pointer_cast<detail::Waker>(task));
        detail::TaskBase::current = task.get();
        bool done = task->poll(ctx);
        detail::TaskBase::current = nullptr;

        if (done) {
            task->scheduling_state.store(
                detail::SchedulingState::Done, std::memory_order_relaxed);
            {
                std::lock_guard lock(m_owned_mutex);
                m_owned_tasks.erase(task);
            }
        } else {
            expected = detail::SchedulingState::Running;
            if (task->scheduling_state.compare_exchange_strong(
                    expected, detail::SchedulingState::Idle,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                task.reset();  // executor's owned map keeps the task alive while parked
            } else {
                if (expected != detail::SchedulingState::RunningAndNotified) {
                    std::cerr << "[coro] SingleThreadedUvExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " after Running→Idle CAS failure\n";
                    std::abort();
                }
                if (!task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::Notified,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    std::cerr << "[coro] SingleThreadedUvExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " during RunningAndNotified→Notified transition\n";
                    std::abort();
                }
                m_ready.push(std::move(task));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Thread-local access
// ---------------------------------------------------------------------------

void set_current_uv_executor(SingleThreadedUvExecutor* exec) {
    t_current_uv_executor = exec;
}

SingleThreadedUvExecutor& current_uv_executor() {
    if (!t_current_uv_executor)
        throw std::runtime_error(
            "coro::current_uv_executor(): no uv executor active on this thread");
    return *t_current_uv_executor;
}

} // namespace coro
