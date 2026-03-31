#pragma once

#include <coro/detail/waker.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace coro {

/**
 * @brief Background timer thread that fires @ref Waker callbacks at requested deadlines.
 *
 * `Runtime` owns one `TimerService`. Leaf futures that need to wake at a specific time
 * (e.g. @ref SleepFuture) call `schedule_wakeup()` from their `poll()` method to register
 * a deadline + waker pair. The timer thread sleeps until the next deadline, then calls
 * `Waker::wake()` to move the task back into the executor's ready queue.
 *
 * Wakers are fired outside the internal mutex to avoid lock-order issues with the executor.
 *
 * Intended as a temporary stand-in until libuv timer integration is added, at which
 * point `SleepFuture::poll()` will register a libuv timer instead of calling this service.
 */
class TimerService {
public:
    TimerService();
    ~TimerService();

    TimerService(const TimerService&)            = delete;
    TimerService& operator=(const TimerService&) = delete;

    /// @brief Registers a waker to be fired at `deadline`.
    /// Thread-safe; may be called from any thread.
    void schedule(std::chrono::steady_clock::time_point deadline,
                  std::shared_ptr<detail::Waker> waker);

private:
    struct Entry {
        std::chrono::steady_clock::time_point deadline;
        std::shared_ptr<detail::Waker>        waker;

        // Reversed so priority_queue gives us a min-heap (earliest deadline at top).
        bool operator>(const Entry& o) const { return deadline > o.deadline; }
    };

    void timer_loop();

    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> m_queue;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    bool                    m_stop{false};
    std::thread             m_thread;
};


/// @brief Sets the thread-local current timer service. Called by `Runtime::block_on()` and worker threads.
void set_current_timer_service(TimerService* ts);

/// @brief Schedules `waker` to be fired at `deadline` on the current runtime's timer service.
/// May only be called from within a `Runtime::block_on()` context.
void schedule_wakeup(std::chrono::steady_clock::time_point deadline,
                     std::shared_ptr<detail::Waker> waker);

} // namespace coro
