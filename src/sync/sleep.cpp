#include <coro/sync/sleep.h>
#include <coro/runtime/runtime.h>

namespace coro {

PollResult<void> SleepFuture::poll(detail::Context& cx) {
    if (Clock::now() >= m_deadline) {
        // Usually stale by now, which cancel ignores. Not if this poll was caused by
        // something else (a spurious wake, a sibling under select): then it stops the
        // timer from waking the task again later.
        cancel_timer();
        return PollReady;
    }

    detail::Weak<detail::Waker> waker = cx.get_weak_waker();
    if (m_runtime) {
        // Woken by the runtime shutting down, not by the timer: the queue is closed
        // and nothing will fire. Only reachable when this future is polled by
        // something the runtime does not own (a thread outside it, a task of another
        // runtime); the runtime's own tasks have all finished by then.
        //
        // Race (handled): the flag is set before the queue wakes its timers, so the
        // poll that follows that wake sees it. A poll that read it just before goes
        // on to return pending, and is then woken.
        if (m_runtime->io_shut_down()) {
            cancel_timer();   // a stale id by now; just forgets the registration
            return PollError(Runtime::shut_down_error());
        }
        // Re-polled before the deadline. Nearly always by the same task, whose timer
        // is still in the queue: nothing to do, and no lock taken.
        if (detail::same_rc(m_waker, waker)) return PollPending;
        // A different waker: the coroutine frame this future lives in has changed
        // hands (see doc/design/timers.md, "SleepFuture"). Move the timer to it.
        //
        // Race (handled): the queue may fire the old timer at any point here. The
        // cancel is then a no-op and the old waker's wake is spurious. The timer added
        // below is already due, so the next turn fires it.
        cancel_timer();
    }
    Runtime& runtime = current_runtime();
    m_timer   = runtime.add_timer(m_deadline, waker);   // may throw; nothing registered then
    m_runtime = &runtime;
    m_waker   = std::move(waker);
    return PollPending;
}

void SleepFuture::cancel_timer() noexcept {
    if (!m_runtime) return;
    // Race (benign): the queue may have taken the waker just before this, and wake
    // the task after the future is gone. Tasks tolerate spurious wakes.
    m_runtime->cancel_timer(m_timer);
    m_runtime = nullptr;
    m_waker   = {};
}

} // namespace coro
