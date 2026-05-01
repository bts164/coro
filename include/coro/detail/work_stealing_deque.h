#pragma once

// Internal — not part of the public API.
// Per-worker FIFO task queue with a work-stealing interface.
//
// Currently backed by std::mutex + std::deque for correctness and simplicity.
//
//   push        — owner thread: enqueue to back
//   pop         — owner thread: dequeue from front (FIFO)
//   steal       — thief thread: dequeue from front (same end as pop)
//
// Because both pop and steal consume from the front, the owner and thieves
// compete for the same tasks and ordering is strictly FIFO. This differs from
// a Chase-Lev deque where the owner pops from the back (LIFO) and thieves
// steal from the front.

#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace coro::detail {

template<typename T>
class WorkStealingDeque {
public:
    /// @brief Push an item onto the back. Called only by the owning worker.
    void push(T item) {
        std::lock_guard lock(m_mutex);
        m_deque.push_back(std::move(item));
    }

    /// @brief Pop the oldest item from the front. Called only by the owning worker.
    /// @return The item, or std::nullopt if the deque is empty.
    std::optional<T> pop() {
        std::lock_guard lock(m_mutex);
        if (m_deque.empty()) return std::nullopt;
        T item = std::move(m_deque.front());
        m_deque.pop_front();
        return item;
    }

    /// @brief Steal an item from the front. May be called by any thread.
    /// @return The item, or std::nullopt if the deque is empty.
    std::optional<T> steal() {
        std::lock_guard lock(m_mutex);
        if (m_deque.empty()) return std::nullopt;
        T item = std::move(m_deque.front());
        m_deque.pop_front();
        return item;
    }

    /// @brief Steal approximately half the items into `dst`. May be called by any thread.
    ///
    /// Acquires only the victim's lock; releases it before pushing into `dst` to avoid
    /// nested locking (victim → dst lock order would deadlock with a concurrent reverse
    /// steal). The first stolen item is returned directly so the caller can run it
    /// immediately without an extra `dst.pop()` round-trip.
    ///
    /// @return The first stolen item, or std::nullopt if the victim was empty.
    std::optional<T> steal_half(WorkStealingDeque& dst) {
        std::vector<T> stolen;
        {
            std::lock_guard lock(m_mutex);
            if (m_deque.empty()) return std::nullopt;
            const std::size_t n = (m_deque.size() + 1) / 2;
            stolen.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                stolen.push_back(std::move(m_deque.front()));
                m_deque.pop_front();
            }
        }
        // Push all but the first into dst; return the first to run immediately.
        for (std::size_t i = 1; i < stolen.size(); ++i)
            dst.push(std::move(stolen[i]));
        return std::move(stolen[0]);
    }

    bool empty() const {
        std::lock_guard lock(m_mutex);
        return m_deque.empty();
    }

private:
    mutable std::mutex m_mutex;
    std::deque<T>      m_deque;
};

} // namespace coro::detail
