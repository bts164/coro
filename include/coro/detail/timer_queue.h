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
 * @brief Names one timer in a TimerQueue: its slot's index in the low 32 bits and
 * that slot's generation in the high 32.
 *
 * An id goes stale when its timer fires or is cancelled. A stale id never matches a
 * later timer in the same slot, because freeing a slot bumps its generation.
 */
using TimerId = std::uint64_t;

/**
 * @brief A binary min-heap of {deadline, slot}, plus the "a thread is blocked
 * waiting for the earliest deadline" record that tells insert() when to unpark it.
 *
 * A timer is one heap entry and one slot, both in vectors the queue owns, so adding
 * one allocates nothing once those have grown. cancel() is lazy: it empties the slot
 * and leaves the heap entry to be popped at its deadline, or removed by a sweep once
 * cancelled entries outnumber live ones.
 *
 * Thread-safe: one mutex guards everything, and it is never held while a waker is
 * woken or any other lock is taken.
 */
class TimerQueue {
public:
    /// A sweep needs at least this many cancelled entries, so a small queue doesn't
    /// rebuild its heap every few cancels.
    static constexpr std::size_t kSweepMinCancelled = 64;

    struct Inserted {
        TimerId id;
        /// True if a thread is blocked in begin_wait()/end_wait() and the new deadline
        /// is earlier than the one it is waiting for, so the caller must unpark it.
        bool    unpark;
    };

    /// @brief Adds a timer that wakes `waker` once `deadline` has passed.
    Inserted insert(Instant deadline, Weak<Waker> waker);

    /**
     * @brief Cancels a timer, so that it fires nothing.
     *
     * Does nothing if `id` is stale: the timer has already fired or been cancelled.
     * The heap entry stays until its deadline, unless this cancel makes the cancelled
     * entries outnumber the live ones (and reach kSweepMinCancelled), in which case
     * it removes them all before returning.
     */
    void cancel(TimerId id) noexcept;

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
     * insert() made meanwhile reports `unpark`, and that unpark makes the next turn
     * return at once (one extra turn, the same benign race end_wait() already has).
     *
     * @return The number of wakers fired.
     */
    std::size_t end_wait_and_fire_expired();

    /// Entries in the heap, including cancelled ones not yet popped or swept.
    std::size_t size() const;

private:
    struct Entry {
        Instant       deadline;
        std::uint64_t seq;    // insertion order: equal deadlines fire FIFO
        std::uint32_t slot;   // index into m_slots
    };

    // In use from insert() until its entry leaves the heap, then on m_free_slots.
    struct Slot {
        // Weak, as in ScheduledIo: a strong waker would keep a finished task alive
        // until its deadline. Empty once cancelled.
        Weak<Waker>   waker;
        std::uint32_t generation = 0;
        bool          cancelled  = false;
    };

    std::size_t fire_expired(bool end_wait);

    /// Takes a slot off the free list, or adds one. Caller holds m_mutex.
    std::uint32_t take_slot();

    /// Puts a slot whose entry has left the heap back on the free list, and bumps its
    /// generation so that ids issued for it go stale. Caller holds m_mutex.
    void free_slot(std::uint32_t index) noexcept;

    /// Removes every cancelled entry from the heap. Caller holds m_mutex.
    void sweep() noexcept;

    // std::push_heap/pop_heap build a max-heap; "later" puts the earliest on top.
    static bool later(const Entry& a, const Entry& b) {
        if (a.deadline != b.deadline) return a.deadline > b.deadline;
        return a.seq > b.seq;
    }

    mutable Mutex              m_mutex;
    // Everything below is GUARDED BY m_mutex.
    std::vector<Entry>         m_heap;
    std::vector<Slot>          m_slots;
    // Kept with capacity for every slot, so that freeing one never allocates.
    std::vector<std::uint32_t> m_free_slots;
    std::uint64_t              m_next_seq  = 0;
    // Cancelled entries still in the heap.
    std::size_t                m_cancelled = 0;
    // True between a blocking begin_wait() and end_wait().
    bool                       m_waiting   = false;
};

} // namespace coro::detail
