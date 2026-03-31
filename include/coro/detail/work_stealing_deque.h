#pragma once

// Internal — not part of the public API.
// Per-worker task deque with a Chase-Lev style interface.
//
// Currently backed by std::mutex + std::deque for correctness and simplicity.
// The interface (push / pop / steal) is compatible with a future lock-free
// Chase-Lev implementation — callers will not need to change.
//
//   push / pop  — owner thread operations (push to back, pop from back)
//   steal       — thief thread operation  (steal from front)

#include <deque>
#include <mutex>
#include <optional>

namespace coro::detail {

template<typename T>
class WorkStealingDeque {
public:
    /// @brief Push an item onto the back. Called only by the owning worker.
    void push(T item) {
        std::lock_guard lock(m_mutex);
        m_deque.push_back(std::move(item));
    }

    /// @brief Pop an item from the back. Called only by the owning worker.
    /// @return The item, or std::nullopt if the deque is empty.
    std::optional<T> pop() {
        std::lock_guard lock(m_mutex);
        if (m_deque.empty()) return std::nullopt;
        T item = std::move(m_deque.back());
        m_deque.pop_back();
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

    bool empty() const {
        std::lock_guard lock(m_mutex);
        return m_deque.empty();
    }

private:
    mutable std::mutex m_mutex;
    std::deque<T>      m_deque;
};

} // namespace coro::detail
