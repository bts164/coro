#include <coro/runtime/single_threaded_uv_executor.h>
#include <coro/runtime/task_waker.h>
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
}

SingleThreadedUvExecutor::~SingleThreadedUvExecutor() {
    stop();
}

// ---------------------------------------------------------------------------
// Executor interface
// ---------------------------------------------------------------------------

void SingleThreadedUvExecutor::schedule(std::unique_ptr<detail::Task> task) {
    auto shared = std::shared_ptr<detail::Task>(std::move(task));
    shared->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    enqueue(std::move(shared));
}

void SingleThreadedUvExecutor::enqueue(std::shared_ptr<detail::Task> task) {
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

// ---------------------------------------------------------------------------
// IoRequest submission
// ---------------------------------------------------------------------------

void SingleThreadedUvExecutor::submit(std::unique_ptr<IoRequest> req) {
    {
        std::lock_guard lk(m_io_queue_mutex);
        m_io_queue.push_back(std::move(req));
    }
    uv_async_send(&m_async);
}

void SingleThreadedUvExecutor::stop() {
    if (m_stopping.exchange(true))
        return;  // idempotent

    uv_async_send(&m_async);  // wake so io_async_cb sees m_stopping

    if (m_uv_thread.joinable())
        m_uv_thread.join();

    if (uv_loop_close(&m_uv_loop) == UV_EBUSY) {
        uv_run(&m_uv_loop, UV_RUN_DEFAULT);
        uv_loop_close(&m_uv_loop);
    }
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
    self->process_io_queue();
    self->drain_ready_tasks();

    if (self->m_stopping.load()) {
        if (self->m_lws_ctx) {
            lws_context_destroy(self->m_lws_ctx);
            self->m_lws_ctx = nullptr;
        }
        uv_close(reinterpret_cast<uv_handle_t*>(handle), nullptr);
        return;
    }

    // If tasks were woken during drain_ready_tasks (pushed to m_ready via
    // enqueue() on the uv thread), schedule another callback to drain them.
    if (!self->m_ready.empty()) {
        uv_async_send(handle);
    }
}

void SingleThreadedUvExecutor::io_thread_loop() {
    m_uv_thread_id = std::this_thread::get_id();
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
    std::deque<std::shared_ptr<detail::Task>> local;
    {
        std::lock_guard lock(m_remote_mutex);
        std::swap(local, m_incoming_wakes);
    }
    for (auto& t : local)
        m_ready.push(std::move(t));
}

void SingleThreadedUvExecutor::process_io_queue() {
    std::deque<std::unique_ptr<IoRequest>> local;
    {
        std::lock_guard lk(m_io_queue_mutex);
        std::swap(local, m_io_queue);
    }
    for (auto& req : local)
        req->execute(&m_uv_loop);
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

        auto waker      = std::make_shared<TaskWaker>();
        waker->task     = task;
        waker->executor = this;
        detail::Context ctx(waker);
        detail::Task::current = task.get();
        bool done = task->poll(ctx);
        detail::Task::current = nullptr;

        if (done) {
            task->scheduling_state.store(
                detail::SchedulingState::Done, std::memory_order_relaxed);
        } else {
            expected = detail::SchedulingState::Running;
            if (task->scheduling_state.compare_exchange_strong(
                    expected, detail::SchedulingState::Idle,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                task.reset();  // waker holds the only ref; parked until wake() fires
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
