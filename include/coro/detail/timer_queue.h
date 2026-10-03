#pragma once

// Deadline-ordered timer queue shared by the IoDriver (desktop) and
// CurrentThreadExecutor (Pico, and standalone use). See doc/design/io_driver.md,
// "Timers".

#include <coro/runtime/clock.h>
#include <coro/detail/mutex.h>
#include <coro/detail/rc.h>
#include <coro/detail/waker.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace coro::detail {

/**
 * @brief The state a SleepFuture shares with its TimerQueue entry.
 *
 * The future stores its waker here on every pending poll and empties it when
 * dropped; the queue takes the waker out when the deadline passes. An entry whose
 * slot is empty is popped without a wake (lazy cancellation).
 */
struct TimerSlot {
    Mutex       mutex;
    // Weak, as in ScheduledIo: the task owns the future, which owns this slot, so a
    // strong waker here would be a reference cycle through the queue.
    // GUARDED BY mutex.
    Weak<Waker> waker;
};

/**
 * @brief A binary min-heap of {deadline, slot}, plus the "a thread is blocked
 * waiting for the earliest deadline" record that tells insert() when to unpark it.
 *
 * Thread-safe. Lock order: the queue's mutex may be held while taking a slot's
 * mutex, never the other way round.
 */
class TimerQueue {
public:
    /**
     * @brief Adds an entry for `slot` at `deadline`.
     *
     * @return true if a thread is blocked in begin_wait()/end_wait() and `deadline`
     *         is earlier than every entry it computed its timeout from, so the caller
     *         must unpark it.
     */
    bool insert(Instant deadline, Rc<TimerSlot> slot);

    /**
     * @brief For a thread about to block for up to `max_wait` (nullopt = no limit).
     *
     * @return `max_wait` bounded by the time to the earliest deadline, rounded up
     *         (zero if it has already passed). If the result is non-zero, records that
     *         a waiter is blocked until end_wait(), so insert() reports earlier entries.
     */
    std::optional<std::chrono::nanoseconds>
        begin_wait(std::optional<std::chrono::nanoseconds> max_wait);

    /// Clears the record set by begin_wait().
    void end_wait();

    /**
     * @brief Pops every entry whose deadline has passed and wakes the live ones,
     * outside the queue's lock.
     *
     * @return The number of wakers fired.
     */
    std::size_t fire_expired();

    /**
     * @brief end_wait() then fire_expired(), under one lock instead of two.
     *
     * For IoDriver, which runs both on every turn. It dispatches I/O events before
     * calling this, so the wait stays recorded through that dispatch: an earlier
     * insert() made meanwhile reports true, and its unpark makes the next turn return
     * at once (one extra turn, the same benign race end_wait() already has).
     *
     * @return The number of wakers fired.
     */
    std::size_t end_wait_and_fire_expired();

    /// Entries in the heap, including cancelled ones not yet popped.
    std::size_t size() const;

private:
    struct Entry {
        Instant       deadline;
        uint64_t      seq;   // insertion order: equal deadlines fire FIFO
        Rc<TimerSlot> slot;
    };

    // std::push_heap/pop_heap build a max-heap; "later" puts the earliest on top.
    std::size_t fire_expired(bool end_wait);

    static bool later(const Entry& a, const Entry& b) {
        if (a.deadline != b.deadline) return a.deadline > b.deadline;
        return a.seq > b.seq;
    }

    mutable Mutex      m_mutex;
    std::vector<Entry> m_heap;          // GUARDED BY m_mutex
    uint64_t           m_next_seq = 0;  // GUARDED BY m_mutex
    // True between a blocking begin_wait() and end_wait(). GUARDED BY m_mutex.
    bool               m_waiting  = false;
};

} // namespace coro::detail
