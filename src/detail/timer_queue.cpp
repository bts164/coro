#include <coro/detail/timer_queue.h>

#include <algorithm>
#include <mutex>
#include <utility>

namespace coro::detail {

bool TimerQueue::insert(Instant deadline, Rc<TimerSlot> slot) {
    std::lock_guard lock(m_mutex);
    // The waiter's timeout was computed from the front entry, or is unbounded if the
    // heap was empty. Entries are popped only by the thread that turns (after its
    // end_wait()), so while it waits the front can only get earlier, via inserts that
    // each saw m_waiting. A deadline earlier than the current front is therefore
    // earlier than the one the waiter will wake for.
    const bool earliest = m_heap.empty() || deadline < m_heap.front().deadline;
    m_heap.push_back(Entry{deadline, m_next_seq++, std::move(slot)});
    std::push_heap(m_heap.begin(), m_heap.end(), later);
    return m_waiting && earliest;
}

std::optional<std::chrono::nanoseconds>
TimerQueue::begin_wait(std::optional<std::chrono::nanoseconds> max_wait) {
    if (max_wait && max_wait->count() <= 0) return std::chrono::nanoseconds::zero();

    std::lock_guard lock(m_mutex);
    std::optional<std::chrono::nanoseconds> wait = max_wait;
    if (!m_heap.empty()) {
        const Instant now      = Clock::now();
        const Instant deadline = m_heap.front().deadline;
        // ceil: waking a hair after the deadline fires it; a hair before would cost
        // a second turn.
        const auto until = deadline <= now
            ? std::chrono::nanoseconds::zero()
            : std::chrono::ceil<std::chrono::nanoseconds>(deadline - now);
        if (!wait || until < *wait) wait = until;
    }
    // Recorded under the same lock insert() reads it with: either this computation
    // saw an entry, or that insert() sees m_waiting and its caller unparks. An unpark
    // made before the waiter actually blocks is not lost: the driver's eventfd stays
    // readable until its poll consumes it.
    m_waiting = !wait || wait->count() > 0;
    return wait;
}

void TimerQueue::end_wait() {
    std::lock_guard lock(m_mutex);
    // Race (benign): an insert() between the waiter's wake-up and here still returns
    // true; its unpark makes the NEXT turn return at once. One extra iteration.
    m_waiting = false;
}

std::size_t TimerQueue::fire_expired() {
    return fire_expired(false);
}

std::size_t TimerQueue::end_wait_and_fire_expired() {
    return fire_expired(true);
}

std::size_t TimerQueue::fire_expired(bool end_wait) {
    std::vector<Rc<Waker>> wakers;
    {
        std::lock_guard lock(m_mutex);
        // As in end_wait(), including its benign race.
        if (end_wait) m_waiting = false;
        if (m_heap.empty()) return 0;
        const Instant now = Clock::now();
        while (!m_heap.empty() && m_heap.front().deadline <= now) {
            std::pop_heap(m_heap.begin(), m_heap.end(), later);
            Rc<TimerSlot> slot = std::move(m_heap.back().slot);
            m_heap.pop_back();
            Rc<Waker> waker;
            {
                std::lock_guard slot_lock(slot->mutex);
                waker = std::exchange(slot->waker, {}).lock();
            }
            if (waker) wakers.push_back(std::move(waker));
        }
    }
    // Wake outside the lock: wake() enqueues into an executor and takes its locks.
    //
    // Race (benign): the future may be dropped, or re-polled with a new waker on
    // another worker, between the slot unlock above and these calls. The wake is then
    // spurious, and the weak waker keeps it from touching a freed task. No wake is
    // lost: an entry is popped only once its deadline has passed, and the clock is
    // monotonic, so every later poll() of the future sees the deadline passed and
    // returns ready without needing its slot again.
    for (auto& waker : wakers) waker->wake();
    return wakers.size();
}

std::size_t TimerQueue::size() const {
    std::lock_guard lock(m_mutex);
    return m_heap.size();
}

} // namespace coro::detail
