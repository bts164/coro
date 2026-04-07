#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/io_service.h>
#include <coro/runtime/task_waker.h>
#include <coro/detail/context.h>
#include <cstdlib>
#include <iostream>

namespace coro {

// Thread-locals identifying which WorkSharingExecutor owns this thread and
// which worker slot it occupies. Both must be checked in enqueue() to avoid
// routing a cross-executor wake to the wrong local queue.
thread_local WorkSharingExecutor* t_owning_executor = nullptr;
thread_local int                  t_worker_index    = -1;

WorkSharingExecutor::WorkSharingExecutor(Runtime* runtime, std::size_t num_threads)
    : m_local_queues(num_threads)
    , m_runtime(runtime)
{
    m_workers.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
        m_workers.emplace_back([this, i] { worker_loop(static_cast<int>(i)); });
}

WorkSharingExecutor::~WorkSharingExecutor() {
    {
        std::lock_guard lock(m_mutex);
        // RACE CONDITION NOTE: m_stop must be set *inside* m_mutex before notify_all().
        // If set outside the lock, a worker can evaluate the cv predicate as false,
        // then we set m_stop=true and call notify_all(), and the worker enters wait()
        // and sleeps forever (lost wakeup).
        m_stop = true;
    }
    m_cv.notify_all();
    for (auto& t : m_workers)
        t.join();
}

void WorkSharingExecutor::schedule(std::unique_ptr<detail::Task> task) {
    auto shared = std::shared_ptr<detail::Task>(std::move(task));
    shared->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    enqueue(std::move(shared));
}

void WorkSharingExecutor::enqueue(std::shared_ptr<detail::Task> task) {
    const int idx = t_worker_index;
    if (idx >= 0 && t_owning_executor == this) {
        // Local path: this is a worker of *this* executor — push to its own queue, no lock.
        m_local_queues[idx].push(std::move(task));
    } else {
        // Remote path: external or main thread — use shared injection queue.
        {
            std::lock_guard lock(m_mutex);
            m_injection_queue.push_back(std::move(task));
        }
        m_cv.notify_one();
    }
}

void WorkSharingExecutor::wait_for_completion(detail::TaskStateBase& state) {
    state.wait_until_done();
}

void WorkSharingExecutor::worker_loop(int worker_index) {
    t_owning_executor = this;
    t_worker_index    = worker_index;
    set_current_runtime(m_runtime);
    set_current_io_service(&m_runtime->io_service());

    while (true) {
        std::shared_ptr<detail::Task> task;

        // Try the local queue first — no lock needed.
        if (auto local = m_local_queues[worker_index].pop()) {
            task = std::move(*local);
        } else {
            // Local queue empty — wait for the injection queue or shutdown.
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] {
                return !m_injection_queue.empty() || m_stop;
            });
            if (!m_injection_queue.empty()) {
                task = std::move(m_injection_queue.front());
                m_injection_queue.pop_front();
            }
            // lock released here
        }

        if (!task) {
            // m_stop was set and injection queue was empty. Check local queue
            // one more time (a concurrent enqueue may have just pushed to it).
            if (auto local = m_local_queues[worker_index].pop())
                task = std::move(*local);
            else
                break; // truly nothing left — exit
        }

        // CAS Notified → Running. Failure is a bug (double-dequeue or bad state).
        auto expected = detail::SchedulingState::Notified;
        if (!task->scheduling_state.compare_exchange_strong(
                expected, detail::SchedulingState::Running,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            std::cerr << "[coro] WorkSharingExecutor: unexpected scheduling_state "
                      << static_cast<int>(expected)
                      << " during Notified→Running transition (expected Notified=2)\n";
            std::abort();
        }

        auto waker = std::make_shared<TaskWaker>();
        waker->task     = task;
        waker->executor = this;
        detail::Context ctx(waker);
        detail::Task::current = task.get();
        bool done = task->poll(ctx);
        detail::Task::current = nullptr;

        if (done) {
            task->scheduling_state.store(
                detail::SchedulingState::Done, std::memory_order_relaxed);
            task.reset(); // destructor fires here, outside any lock
        } else {
            // Try Running → Idle: park the task, waker owns the only ref.
            expected = detail::SchedulingState::Running;
            if (task->scheduling_state.compare_exchange_strong(
                    expected, detail::SchedulingState::Idle,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                task.reset(); // waker holds the only ref; parks until wake() fires
            } else {
                // CAS failed: expected now holds the actual state. The only valid
                // state here is RunningAndNotified — wake() fired during poll().
                if (expected != detail::SchedulingState::RunningAndNotified) {
                    std::cerr << "[coro] WorkSharingExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " after Running→Idle CAS failure (expected RunningAndNotified=3)\n";
                    std::abort();
                }
                if (!task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::Notified,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    std::cerr << "[coro] WorkSharingExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " during RunningAndNotified→Notified transition\n";
                    std::abort();
                }
                // Re-enqueue via local path (we are a worker thread).
                enqueue(std::move(task));
            }
        }
    }

    set_current_runtime(nullptr);
    set_current_io_service(nullptr);
    t_owning_executor = nullptr;
    t_worker_index    = -1;
}

} // namespace coro
