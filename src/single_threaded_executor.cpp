#include <coro/single_threaded_executor.h>
#include <coro/context.h>

namespace coro {

namespace {

struct TaskWaker final : Waker {
    detail::Task*           key;
    SingleThreadedExecutor* executor;

    void wake() override {
        executor->wake_task(key);
    }

    std::shared_ptr<Waker> clone() override {
        return std::make_shared<TaskWaker>(*this);
    }
};

} // namespace

SingleThreadedExecutor::SingleThreadedExecutor() = default;
SingleThreadedExecutor::~SingleThreadedExecutor() = default;

void SingleThreadedExecutor::schedule(std::unique_ptr<detail::Task> task) {
    m_ready.push(std::shared_ptr<detail::Task>(std::move(task)));
}

void SingleThreadedExecutor::wake_task(detail::Task* key) {
    if (key == m_running_task_key) {
        // Self-wake: task is currently inside poll(). Mark it for re-enqueue
        // after poll() returns instead of searching m_suspended.
        m_running_task_woken = true;
        return;
    }
    auto it = m_suspended.find(key);
    if (it != m_suspended.end()) {
        m_ready.push(std::move(it->second));
        m_suspended.erase(it);
    }
    // Not found: task is already scheduled or complete — no-op.
}

bool SingleThreadedExecutor::poll_ready_tasks() {
    if (m_ready.empty()) return false;

    // Snapshot count so tasks enqueued by synchronous wakers during this pass
    // are deferred to the next call, preventing unbounded looping.
    const auto count = m_ready.size();
    for (std::size_t i = 0; i < count && !m_ready.empty(); ++i) {
        auto task = std::move(m_ready.front());
        m_ready.pop();

        m_running_task_key   = task.get();
        m_running_task_woken = false;

        auto waker = make_waker(task.get());
        Context ctx(waker);
        bool done = task->poll(ctx);

        m_running_task_key = nullptr;

        if (!done) {
            if (m_running_task_woken) {
                // wake() was called during poll() — re-enqueue for the next pass.
                m_ready.push(std::move(task));
            } else {
                // Task is waiting on an external event — park it in m_suspended.
                m_suspended[task.get()] = std::move(task);
            }
        }
        // done == true: task completed, drop it here.
    }
    return true;
}

bool SingleThreadedExecutor::empty() const {
    return m_ready.empty();
}

std::shared_ptr<Waker> SingleThreadedExecutor::make_waker(detail::Task* key) {
    auto w      = std::make_shared<TaskWaker>();
    w->key      = key;
    w->executor = this;
    return w;
}

} // namespace coro
