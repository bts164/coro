#include <coro/sync/sleep.h>
#include <coro/runtime/runtime.h>

#include <mutex>

namespace coro {

PollResult<void> SleepFuture::poll(detail::Context& cx) {
    if (Clock::now() >= m_deadline) {
        release();
        return PollReady;
    }
    if (!m_slot) {
        // Not shared until add_timer() publishes it, so no lock is needed yet.
        auto slot   = detail::make_rc<detail::TimerSlot>();
        slot->waker = cx.get_weak_waker();
        current_runtime().add_timer(m_deadline, slot);   // may throw; m_slot stays null
        m_slot = std::move(slot);
    } else {
        // Re-polled before the deadline, possibly with a different waker (select),
        // possibly on another worker.
        {
            std::lock_guard lock(m_slot->mutex);
            m_slot->waker = cx.get_weak_waker();
        }
        // Race (handled): the queue may have popped this entry, taking the OLD waker,
        // between the clock check above and the store. The new waker would then
        // never fire. The queue pops only after the deadline has passed (and took the
        // slot mutex to do so), so this second check sees it passed.
        if (Clock::now() >= m_deadline) {
            release();
            return PollReady;
        }
    }
    return PollPending;
}

void SleepFuture::release() noexcept {
    if (!m_slot) return;
    {
        // Race (benign): the queue may have taken the waker just before this, and
        // wake the task after the future is gone. Tasks tolerate spurious wakes.
        std::lock_guard lock(m_slot->mutex);
        m_slot->waker.reset();
    }
    m_slot = nullptr;
}

} // namespace coro
