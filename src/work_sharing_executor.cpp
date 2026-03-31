#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/runtime.h>
#include <coro/detail/context.h>

namespace coro {

namespace {

struct TaskWaker final : detail::Waker {
    detail::Task*         key;
    WorkSharingExecutor*  executor;

    void wake() override {
        executor->wake_task(key);
    }

    std::shared_ptr<Waker> clone() override {
        return std::make_shared<TaskWaker>(*this);
    }
};

} // namespace

WorkSharingExecutor::WorkSharingExecutor(std::size_t num_threads, Runtime* runtime)
    : m_runtime(runtime)
{
    m_workers.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
        m_workers.emplace_back([this] { worker_loop(); });
}

WorkSharingExecutor::~WorkSharingExecutor() {
    {
        std::lock_guard lock(m_mutex);
        // RACE CONDITION NOTE: m_stop must be set *inside* m_mutex before notify_all().
        // If set outside the lock, a worker can evaluate the cv predicate (!m_queue.empty() || m_stop)
        // as false, then we set m_stop=true and call notify_all(), and the worker then enters wait()
        // and sleeps forever (lost wakeup). Holding the lock while setting m_stop closes this window.
        m_stop = true;
    }
    m_cv.notify_all();
    for (auto& t : m_workers)
        t.join();
}

void WorkSharingExecutor::schedule(std::unique_ptr<detail::Task> task) {
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back(std::shared_ptr<detail::Task>(std::move(task)));
    }
    m_cv.notify_one();
}

bool WorkSharingExecutor::poll_ready_tasks() {
    std::lock_guard lock(m_mutex);
    return !m_queue.empty();
}

void WorkSharingExecutor::wait_for_completion(detail::TaskStateBase& state) {
    state.wait_until_done();
}

void WorkSharingExecutor::wake_task(detail::Task* key) {
    std::lock_guard lock(m_mutex);

    // If the task is currently Running (in m_self_woken), set the self-wake flag.
    auto self_it = m_self_woken.find(key);
    if (self_it != m_self_woken.end()) {
        self_it->second = true;
        return;
    }

    // Otherwise move from Suspended to Ready and wake a worker.
    auto sus_it = m_suspended.find(key);
    if (sus_it != m_suspended.end()) {
        m_queue.push_back(std::move(sus_it->second));
        m_suspended.erase(sus_it);
        m_cv.notify_one();
    }
    // Not found: task is already Ready, Running, or Done — no-op.
}

void WorkSharingExecutor::worker_loop() {
    set_current_runtime(m_runtime);

    while (true) {
        std::shared_ptr<detail::Task> task;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] {
                return !m_queue.empty() || m_stop;
            });

            if (m_stop && m_queue.empty())
                break;

            task = std::move(m_queue.front());
            m_queue.pop_front();
            m_self_woken[task.get()] = false;
        }

        // Poll outside the lock so other workers can dequeue and wake_task() can acquire.
        auto w = std::make_shared<TaskWaker>();
        w->key      = task.get();
        w->executor = this;
        detail::Context ctx(w);
        bool done = task->poll(ctx);

        bool self_woke = false;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_self_woken.find(task.get());
            if (it != m_self_woken.end()) {
                self_woke = it->second;
                m_self_woken.erase(it);
            }

            if (done) {
                // RACE CONDITION NOTE: notify_all() must be called *inside* m_mutex.
                // Any waiter that blocks on m_cv must evaluate its predicate under m_mutex.
                // If notify_all() were called after releasing the lock, a waiter could check
                // the predicate (false), then we release + notify, then the waiter enters
                // wait() and sleeps forever (lost wakeup).
                //
                // NOTE: wait_for_completion() no longer blocks on m_cv — it uses
                // state.wait_until_done() which blocks on state.cv (a different condvar).
                // This notify_all is therefore vestigial for that purpose, but the rule
                // above still applies if m_cv is ever used as a completion signal again.
                m_cv.notify_all();
            } else if (self_woke) {
                m_queue.push_back(std::move(task));
                m_cv.notify_one();
            } else {
                m_suspended[task.get()] = std::move(task);
            }
        }

        if (done)
            task.reset(); // destructor fires here, outside the lock
    }

    set_current_runtime(nullptr);
}

} // namespace coro
