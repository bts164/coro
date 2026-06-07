#include <coro/pico/pico_executor.h>
#include <coro/detail/context.h>
#include <coro/detail/waker.h>
#include <pico/cyw43_arch.h>
#include <cstdlib>
#include <iostream>

namespace coro {

void PicoExecutor::schedule(std::shared_ptr<detail::TaskBase> task) {
    task->owning_executor = this;
    task->scheduling_state.store(
        detail::SchedulingState::Notified, std::memory_order_relaxed);
    m_ready.push(std::move(task));
}

// Single-threaded — always called from the executor thread — no lock needed.
void PicoExecutor::enqueue(std::shared_ptr<detail::TaskBase> task) {
    m_ready.push(std::move(task));
}

bool PicoExecutor::poll_ready_tasks() {
    if (m_ready.empty()) return false;

    // Snapshot count so tasks re-enqueued during this pass are deferred to the
    // next call, preventing unbounded looping within a single poll_ready_tasks().
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
            std::cerr << "[coro/pico] PicoExecutor: unexpected scheduling_state "
                      << static_cast<int>(expected)
                      << " during Notified→Running transition\n";
            std::abort();
        }

        // TaskBase IS the Waker — cast directly, no separate waker object needed.
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
                    std::cerr << "[coro/pico] PicoExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " after Running→Idle CAS failure\n";
                    std::abort();
                }
                if (!task->scheduling_state.compare_exchange_strong(
                        expected, detail::SchedulingState::Notified,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    std::cerr << "[coro/pico] PicoExecutor: unexpected scheduling_state "
                              << static_cast<int>(expected)
                              << " during RunningAndNotified→Notified transition\n";
                    std::abort();
                }
                m_ready.push(std::move(task));
            }
        }
    }
    return true;
}

void PicoExecutor::wait_for_completion(detail::TaskStateBase& state) {
    while (true) {
        {
            std::lock_guard lock(state.mutex);
            if (state.terminated) break;
        }
        poll_ready_tasks();
        // Drive WiFi chip and lwIP; fires any pending PicoTcpStream callbacks inline.
        cyw43_arch_poll();
    }
}

} // namespace coro
