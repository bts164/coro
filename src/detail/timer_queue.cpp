#include <coro/detail/timer_queue.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <utility>
#include <vector>

namespace coro::detail {

namespace {

constexpr TimerId make_id(std::uint32_t index, std::uint32_t generation) {
    return (TimerId{generation} << 32) | index;
}

} // namespace

TimerQueue::Inserted TimerQueue::insert(Instant deadline, Weak<Waker> waker) {
    std::lock_guard lock(m_mutex);
    // Race (handled): an insert() that takes the lock before close_and_wake_all() is
    // in the heap that it empties, and is woken; one that takes it after lands here.
    if (m_closed) return Inserted{0, false, true};
    // The waiter's timeout was computed from the front entry, or is unbounded if the
    // heap was empty. Until its end_wait(), the front gets earlier only through
    // inserts, each of which saw m_waiting and so reported it. A sweep can make the
    // front later than the entry the waiter computed from, which only makes this
    // test report more inserts than it needs to. So a deadline that is not earlier
    // than the current front is not earlier than the one the waiter will wake for.
    const bool earliest = m_heap.empty() || deadline < m_heap.front().deadline;
    const std::uint32_t index = take_slot();
    Slot& slot = m_slots[index];
    slot.waker = std::move(waker);
    m_heap.push_back(Entry{deadline, m_next_seq++, index});
    std::push_heap(m_heap.begin(), m_heap.end(), later);
    return Inserted{make_id(index, slot.generation), m_waiting && earliest};
}

void TimerQueue::cancel(TimerId id) noexcept {
    const auto index      = static_cast<std::uint32_t>(id);
    const auto generation = static_cast<std::uint32_t>(id >> 32);
    Weak<Waker> dropped;   // destroyed after the unlock
    {
        std::lock_guard lock(m_mutex);
        if (index >= m_slots.size()) return;
        Slot& slot = m_slots[index];
        // Race (handled): the timer may have fired, and the slot may even hold another
        // timer by now. Either way the slot's generation has moved on from the id's.
        if (slot.generation != generation || slot.cancelled) return;
        dropped        = std::move(slot.waker);
        slot.waker     = {};
        slot.cancelled = true;
        ++m_cancelled;
        // Lazy: the heap entry stays until its deadline. Sweeping only once the
        // cancelled entries outnumber the live ones keeps the heap within about twice
        // the live timers, and means a sweep of n entries follows at least n/2
        // cancels, so its cost per cancel is constant.
        if (m_cancelled >= kSweepMinCancelled && m_cancelled > m_heap.size() - m_cancelled)
            sweep();
    }
}

std::uint32_t TimerQueue::take_slot() {
    if (!m_free_slots.empty()) {
        const std::uint32_t index = m_free_slots.back();
        m_free_slots.pop_back();
        return index;
    }
    const auto index = static_cast<std::uint32_t>(m_slots.size());
    m_slots.emplace_back();
    // free_slot() and sweep() are noexcept: make room for every slot now.
    m_free_slots.reserve(m_slots.capacity());
    return index;
}

void TimerQueue::free_slot(std::uint32_t index) noexcept {
    Slot& slot = m_slots[index];
    slot.waker     = {};
    slot.cancelled = false;
    // A slot would have to be reused 2^32 times while one stale id for it is still
    // held for the generation to wrap round to that id's.
    ++slot.generation;
    m_free_slots.push_back(index);   // capacity reserved by take_slot()
}

void TimerQueue::sweep() noexcept {
    // Each slot has exactly one heap entry, so each cancelled slot is freed once.
    std::erase_if(m_heap, [this](const Entry& entry) {
        if (!m_slots[entry.slot].cancelled) return false;
        free_slot(entry.slot);
        return true;
    });
    std::make_heap(m_heap.begin(), m_heap.end(), later);
    m_cancelled = 0;
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
    // Wakers are taken out under the lock and woken outside it, a fixed-size batch at
    // a time, so that firing allocates nothing however many timers are due.
    constexpr std::size_t kBatch = 32;
    std::array<Rc<Waker>, kBatch> wakers;
    std::size_t fired = 0;
    for (;;) {
        std::size_t count = 0;
        bool more = false;
        {
            std::lock_guard lock(m_mutex);
            // As in end_wait(), including its benign race.
            if (end_wait) m_waiting = false;
            if (m_heap.empty()) break;
            const Instant now = Clock::now();
            while (!m_heap.empty() && m_heap.front().deadline <= now) {
                if (count == kBatch) {
                    more = true;
                    break;
                }
                std::pop_heap(m_heap.begin(), m_heap.end(), later);
                const std::uint32_t index = m_heap.back().slot;
                m_heap.pop_back();
                Slot& slot = m_slots[index];
                if (slot.cancelled) {
                    --m_cancelled;
                } else if (Rc<Waker> waker = slot.waker.lock()) {
                    wakers[count++] = std::move(waker);
                }
                free_slot(index);
            }
        }
        // Wake outside the lock: wake() enqueues into an executor and takes its locks.
        //
        // Race (benign): the future may be dropped between the unlock above and these
        // calls. Its cancel() then finds a stale id and does nothing, and the wake is
        // spurious; the weak waker kept it from touching a freed task. No wake is
        // lost: an entry is popped only once its deadline has passed, and the clock
        // is monotonic, so every later poll() of the future sees the deadline passed
        // and returns ready without needing its timer again.
        for (std::size_t i = 0; i < count; ++i) {
            wakers[i]->wake();
            wakers[i] = nullptr;
        }
        fired += count;
        if (!more) break;
    }
    return fired;
}

std::size_t TimerQueue::close_and_wake_all() {
    // Not batched like fire_expired(): this runs once, at shutdown, where an
    // allocation is no concern.
    std::vector<Rc<Waker>> wakers;
    {
        std::lock_guard lock(m_mutex);
        m_closed = true;
        wakers.reserve(m_heap.size() - m_cancelled);
        for (const Entry& entry : m_heap) {
            Slot& slot = m_slots[entry.slot];
            if (!slot.cancelled) {
                if (Rc<Waker> waker = slot.waker.lock()) wakers.push_back(std::move(waker));
            }
            free_slot(entry.slot);   // bumps the generation: every id is now stale
        }
        m_heap.clear();
        m_cancelled = 0;
    }
    // Wake outside the lock, as fire_expired() does.
    //
    // Race (benign): a future dropped between the unlock and its wake cancels a
    // stale id, which does nothing, and the wake is spurious.
    for (const Rc<Waker>& waker : wakers) waker->wake();
    return wakers.size();
}

std::size_t TimerQueue::size() const {
    std::lock_guard lock(m_mutex);
    return m_heap.size();
}

} // namespace coro::detail
