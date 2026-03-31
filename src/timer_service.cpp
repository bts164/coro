#include <coro/runtime/timer_service.h>
#include <stdexcept>

namespace coro {

namespace {
    thread_local TimerService* t_current_timer_service = nullptr;
} // namespace

TimerService::TimerService()
    : m_thread([this]{ timer_loop(); })
{}

TimerService::~TimerService() {
    {
        std::lock_guard lock(m_mutex);
        // RACE CONDITION NOTE: m_stop must be set inside m_mutex before notify_all().
        // Same pattern as WorkSharingExecutor — see comments there for the full explanation.
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
}

void TimerService::schedule(std::chrono::steady_clock::time_point deadline,
                            std::shared_ptr<detail::Waker> waker) {
    {
        std::lock_guard lock(m_mutex);
        m_queue.push({deadline, std::move(waker)});
    }
    // Notify unconditionally — the timer thread re-evaluates the next deadline after every
    // insertion. If the new entry has an earlier deadline than the current wait target,
    // the thread wakes up and re-arms its wait_until accordingly.
    m_cv.notify_one();
}

void TimerService::timer_loop() {
    std::unique_lock lock(m_mutex);

    while (true) {
        // Wait until there is work to do or we are asked to stop.
        m_cv.wait(lock, [this]{ return m_stop || !m_queue.empty(); });

        if (m_stop) break;

        // Snapshot the earliest deadline and wait until it arrives. The wait is
        // interrupted early (via notify_one) if a new entry with an earlier deadline
        // is added, or if stop is requested.
        auto deadline = m_queue.top().deadline;
        m_cv.wait_until(lock, deadline, [this, &deadline]{
            // Wake early if stopped, or if a new entry arrived before our deadline.
            return m_stop || m_queue.top().deadline < deadline;
        });

        if (m_stop) break;

        // Fire all entries whose deadline has passed. Release the lock while calling
        // wake() to avoid holding it during arbitrary executor code.
        auto now = std::chrono::steady_clock::now();
        while (!m_queue.empty() && m_queue.top().deadline <= now) {
            auto entry = m_queue.top();
            m_queue.pop();
            lock.unlock();
            entry.waker->wake();
            lock.lock();
            now = std::chrono::steady_clock::now();
        }
    }
}

void set_current_timer_service(TimerService* ts) {
    t_current_timer_service = ts;
}

void schedule_wakeup(std::chrono::steady_clock::time_point deadline,
                     std::shared_ptr<detail::Waker> waker) {
    if (!t_current_timer_service)
        throw std::runtime_error("coro::schedule_wakeup(): no runtime active on this thread");
    t_current_timer_service->schedule(deadline, std::move(waker));
}

} // namespace coro
