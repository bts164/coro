#include <coro/runtime/current_thread_executor.h>
#include <coro/detail/context.h>
#include <coro/detail/waker.h>
#include <cstdlib>
#include <iostream>

namespace coro {

void CurrentThreadExecutor::schedule(detail::Rc<detail::TaskBase> task) {
    task->owning_executor = this;
    task->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_owned_mutex);
        m_owned_tasks.insert(task);
    }
    std::lock_guard lock(m_ready_mutex);
    m_ready.push(std::move(task));
}

// May be called from outside the executor thread: an ISR on Pico or an
// external thread on multi-threaded platforms. m_ready_mutex provides the
// appropriate serialisation for the current platform (see detail/mutex.h).
void CurrentThreadExecutor::enqueue(detail::Rc<detail::TaskBase> task) {
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
        detail::Rc<detail::TaskBase> task;
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

        detail::Context ctx(task);
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
                task.reset(); // executor's owned map keeps the task alive while parked
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
                                           detail::Rc<detail::Waker> waker) {
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

#ifdef CORO_PICO
void CurrentThreadExecutor::add_isr_poll(IsrFlagRef                 ref,
                                         detail::Rc<detail::Waker>  waker) {
    m_isr_polls.push_back({ref, std::move(waker)});
}

void CurrentThreadExecutor::remove_isr_poll(IsrFlagRef ref) {
    auto it = std::find_if(m_isr_polls.begin(), m_isr_polls.end(),
                           [ref](const IsrPollEntry& e) { return e.ref.flag == ref.flag; });
    if (it != m_isr_polls.end()) {
        *it = std::move(m_isr_polls.back());
        m_isr_polls.pop_back();
    }
}

void CurrentThreadExecutor::check_isr_events() {
    // Each check takes the flag's paired hardware spin lock before reading it —
    // the same lock the ISR (or the other core) takes to write it. This is what
    // actually makes the read safe against a write happening concurrently on
    // the other core; a bare volatile dereference is not (see
    // doc/design/isr_safety.md, "Cross-core ISR delivery").
    //
    // No payload ordering needed here: we are only deciding whether to fire the
    // waker. The payload itself (IsrChannel<T>) is read inside its own
    // spin-lock critical section in receive(), after co_await returns.
    //
    // Swap-and-pop removes the entry without shifting the remaining vector,
    // keeping the per-iteration cost O(1).
    for (std::size_t i = 0; i < m_isr_polls.size(); ) {
        IsrFlagRef ref = m_isr_polls[i].ref;
        uint32_t save = spin_lock_blocking(ref.lock);
        bool set = *ref.flag;
        spin_unlock(ref.lock, save);
        if (set) {
            auto waker = std::move(m_isr_polls[i].waker);
            m_isr_polls[i] = std::move(m_isr_polls.back());
            m_isr_polls.pop_back();
            waker->wake();
        } else {
            ++i;
        }
    }
}
#endif // CORO_PICO

void CurrentThreadExecutor::wait_for_completion(detail::TaskStateBase& state) {
    while (true) {
        {
            std::lock_guard lock(state.mutex);
            if (state.terminated) break;
        }
        poll_ready_tasks();
        check_expired_timers();
        m_poll();
#ifdef CORO_PICO
        check_isr_events();
#endif
    }
}

} // namespace coro
