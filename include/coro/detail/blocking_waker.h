#pragma once

// Desktop-only: blocking_wait()/blocking_next() (see future.h/stream.h) condvar-block the
// calling OS thread between polls, which has no meaning on CORO_PICO's single-threaded,
// cooperatively-scheduled CurrentThreadExecutor. See doc/design/blocking_wait.md.
#ifndef CORO_PICO

#include <coro/detail/rc.h>
#include <coro/detail/waker.h>
#include <condition_variable>
#include <mutex>

namespace coro::detail {

/**
 * @brief A `Waker` backed by a mutex + condition variable instead of an executor's ready
 * queue. Used by `blocking_wait()`/`blocking_next()` to park the calling OS thread between
 * polls rather than yielding to an executor — see doc/design/blocking_wait.md.
 *
 * `wake()` sets a flag and notifies under the same lock `wait_for_wake()` checks the flag
 * under, so a `wake()` that races ahead of `wait_for_wake()` (fires between `poll()`
 * returning `PollPending` and the next `wait_for_wake()` call) is not missed: the flag is
 * already set by the time the predicate is checked.
 */
class BlockingWaker final : public Waker, public std::enable_shared_from_this<BlockingWaker> {
public:
    void wake() override {
        std::lock_guard lock(m_mutex);
        m_woken = true;
        m_cv.notify_one();
    }

    // shared_from_this() increments the existing refcount — no allocation. Returns the
    // same underlying waker, matching "wakes the same task" for the single caller thread
    // blocking_wait()/blocking_next() drive.
    Rc<Waker> clone() override { return shared_from_this(); }

    /// @brief Blocks the calling thread until `wake()` is called at least once since the
    /// last `wait_for_wake()` returned.
    void wait_for_wake() {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return m_woken; });
        m_woken = false;
    }

private:
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    bool                    m_woken{false};
};

} // namespace coro::detail

#endif // CORO_PICO
