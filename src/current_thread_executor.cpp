#include <coro/runtime/current_thread_executor.h>
#include <coro/detail/context.h>
#include <coro/detail/waker.h>
#include <cstdlib>
#include <iostream>

namespace coro {

void CurrentThreadExecutor::schedule(std::shared_ptr<detail::TaskBase> task) {
    task->owning_executor = this;
    task->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    std::lock_guard lock(m_ready_mutex);
    m_ready.push(std::move(task));
}

// May be called from outside the executor thread: an ISR on Pico or an
// external thread on multi-threaded platforms. m_ready_mutex provides the
// appropriate serialisation for the current platform (see detail/mutex.h).
void CurrentThreadExecutor::enqueue(std::shared_ptr<detail::TaskBase> task) {
    std::lock_guard lock(m_ready_mutex);
    m_ready.push(std::move(task));
}

bool CurrentThreadExecutor::poll_ready_tasks() {
    // Snapshot count under the lock, then pop one task at a time.
    // Holding the lock only for the pop (not across the poll) keeps the
    // ISR-disable window as short as possible.
    std::size_t count;
    {
        std::lock_guard lock(m_ready_mutex);
        count = m_ready.size();
    }
    if (count == 0) return false;

    for (std::size_t i = 0; i < count; ++i) {
        std::shared_ptr<detail::TaskBase> task;
        {
            std::lock_guard lock(m_ready_mutex);
            if (m_ready.empty()) break;
            task = std::move(m_ready.front());
            m_ready.pop();
        }

        auto expected = detail::SchedulingState::Notified;
        if (!task->scheduling_state.compare_exchange_strong(
                expected, detail::SchedulingState::Running,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            std::cerr << "[coro] CurrentThreadExecutor: unexpected scheduling_state "
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
        } else {
            expected = detail::SchedulingState::Running;
            if (task->scheduling_state.compare_exchange_strong(
                    expected, detail::SchedulingState::Idle,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                task.reset(); // waker holds the only ref; parks until wake() fires
            } else {
                // wake() fired during poll() — RunningAndNotified → Notified, re-enqueue
                if (expected != detail::SchedulingState::RunningAndNotified) {
                    std::cerr << "[coro] CurrentThreadExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " after Running→Idle CAS failure\n";
                    std::abort();
                }
                if (!task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::Notified,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    std::cerr << "[coro] CurrentThreadExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " during RunningAndNotified→Notified transition\n";
                    std::abort();
                }
                {
                    std::lock_guard lock(m_ready_mutex);
                    m_ready.push(std::move(task));
                }
            }
        }
    }
    return true;
}

void CurrentThreadExecutor::schedule_timer(uint64_t deadline_us,
                                           std::shared_ptr<detail::Waker> waker) {
    m_timers.push({deadline_us, std::move(waker)});
}

void CurrentThreadExecutor::check_expired_timers() {
    const uint64_t now = m_clock();
    while (!m_timers.empty() && m_timers.top().deadline_us <= now) {
        auto waker = m_timers.top().waker;
        m_timers.pop();
        waker->wake();
    }
}

void CurrentThreadExecutor::wait_for_completion(detail::TaskStateBase& state) {
    while (true) {
        {
            std::lock_guard lock(state.mutex);
            if (state.terminated) break;
        }
        poll_ready_tasks();
        check_expired_timers();
        m_poll();
    }
}

} // namespace coro
