#include <coro/runtime/single_threaded_executor.h>
#include <coro/detail/context.h>
#include <cstdlib>
#include <iostream>

namespace coro {


SingleThreadedExecutor::SingleThreadedExecutor(Runtime* /*runtime*/) :
    m_poll_thread_id() // default-constructed to no thread
{}

SingleThreadedExecutor::~SingleThreadedExecutor() = default;

void SingleThreadedExecutor::schedule(std::shared_ptr<detail::TaskBase> task) {
    task->owning_executor = this;
    task->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_owned_mutex);
        m_owned_tasks.insert(task);
    }
    enqueue(std::move(task));
}

void SingleThreadedExecutor::enqueue(std::shared_ptr<detail::TaskBase> task) {
    if (std::this_thread::get_id() == m_poll_thread_id) {
        // Local path: we are the poll thread — push directly, no lock needed.
        m_ready.push(std::move(task));
    } else {
        // Remote path: external thread — hand off via injection queue.
        //
        // notify_one() is called while still holding m_remote_mutex (rather
        // than after unlocking, which would normally be preferred to avoid
        // waking a thread that immediately blocks on a lock we still hold).
        // wait_for_completion()'s m_remote_cv.wait(lock, pred) cannot
        // reacquire the lock and return until this lock is released, so
        // calling notify_one() here guarantees it has fully returned before
        // the waiting thread can proceed — and, e.g., destroy this executor
        // (and m_remote_cv with it) out from under a still-in-flight
        // notify_one() call from this thread.
        std::lock_guard lock(m_remote_mutex);
        m_incoming_wakes.push_back(std::move(task));
        m_remote_cv.notify_one();
    }
}

bool SingleThreadedExecutor::poll_ready_tasks() {
    // Drain injection queue first so remote wakes are never starved.
    {
        std::lock_guard lock(m_remote_mutex);
        for (auto& t : m_incoming_wakes)
            m_ready.push(std::move(t));
        m_incoming_wakes.clear();
    }

    if (m_ready.empty()) return false;

    // Snapshot count so tasks enqueued by synchronous wakers during this pass
    // are deferred to the next call, preventing unbounded looping.
    const auto count = m_ready.size();
    for (std::size_t i = 0; i < count && !m_ready.empty(); ++i) {
        auto task = std::move(m_ready.front());
        m_ready.pop();

        // CAS Notified → Running. Failure means a bug — two threads dequeuing
        // the same task, or incorrect state management.
        auto expected = detail::SchedulingState::Notified;
        if (!task->scheduling_state.compare_exchange_strong(
                expected, detail::SchedulingState::Running,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            std::cerr << "[coro] SingleThreadedExecutor: unexpected scheduling_state "
                      << static_cast<int>(expected)
                      << " during Notified→Running transition (expected Notified=2)\n";
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
            // Task completed — executor's owned map was the lifetime anchor; now freed.
        } else {
            // Try Running → Idle: park the task; executor's owned map keeps it alive.
            expected = detail::SchedulingState::Running;
            if (task->scheduling_state.compare_exchange_strong(
                    expected, detail::SchedulingState::Idle,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                task.reset(); // release temporary executor ref; task lives via m_owned_tasks
            } else {
                // CAS failed: expected now holds the actual state. The only valid
                // state here is RunningAndNotified — wake() fired during poll().
                if (expected != detail::SchedulingState::RunningAndNotified) {
                    std::cerr << "[coro] SingleThreadedExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " after Running→Idle CAS failure (expected RunningAndNotified=3)\n";
                    std::abort();
                }
                if (!task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::Notified,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    std::cerr << "[coro] SingleThreadedExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " during RunningAndNotified→Notified transition\n";
                    std::abort();
                }
                // Re-enqueue via local path (we are the poll thread).
                m_ready.push(std::move(task));
            }
        }
    }
    return true;
}

void SingleThreadedExecutor::wait_for_completion(detail::TaskStateBase& state) {
    m_poll_thread_id = std::this_thread::get_id();

    while (true) {
        {
            std::lock_guard lock(state.mutex);
            if (state.terminated) break;
        }
        if (poll_ready_tasks()) continue;

        // Ready queue and injection queue are both empty.
        // Block until a remote wake arrives. state.terminated cannot change
        // while blocked here since tasks only complete during poll_ready_tasks().
        std::unique_lock lock(m_remote_mutex);
        m_remote_cv.wait(lock, [this] { return !m_incoming_wakes.empty(); });
    }

    m_poll_thread_id = {};
}

bool SingleThreadedExecutor::empty() const {
    if (!m_ready.empty()) return false;
    std::lock_guard lock(m_remote_mutex);
    return m_incoming_wakes.empty();
}

} // namespace coro
