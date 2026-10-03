#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/detail/context.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

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

    // The uv thread has exited: io_async_cb called uv_stop(), so uv_run() returned
    // without closing anything. Close every remaining handle (m_async, plus any a user
    // left open), run the loop once more to drain their close callbacks, then close
    // the loop.
    uv_walk(&m_uv_loop, [](uv_handle_t* h, void*) {
        if (!uv_is_closing(h))
            uv_close(h, nullptr);
    }, nullptr);

    uv_run(&m_uv_loop, UV_RUN_DEFAULT);
    uv_loop_close(&m_uv_loop);
}

// ---------------------------------------------------------------------------
// uv thread
// ---------------------------------------------------------------------------

void SingleThreadedUvExecutor::io_async_cb(uv_async_t* handle) {
    auto* self = static_cast<SingleThreadedUvExecutor*>(handle->data);

    self->drain_incoming_wakes();
    self->drain_ready_tasks();

    if (self->m_stopping.load()) {
        // uv_stop() rather than closing m_async here: stop() closes every handle
        // once uv_run() has returned.
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
