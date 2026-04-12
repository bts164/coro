#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace coro {

/**
 * @brief Fixed-capacity ring buffer for decoded packets.
 *
 * Provides a FIFO queue with fixed capacity. Used by PollStream to buffer
 * decoded packets between poll_cb (producer) and poll_next (consumer).
 *
 * Thread safety: Not thread-safe. Caller must ensure exclusive access.
 *
 * @tparam T Element type (must be movable)
 */
template<typename T>
class RingBuffer {
public:
    /**
     * @brief Construct a ring buffer with the given capacity.
     * @param capacity Maximum number of elements
     */
    explicit RingBuffer(std::size_t capacity);

    RingBuffer(RingBuffer&&) noexcept = default;
    RingBuffer& operator=(RingBuffer&&) noexcept = default;
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    ~RingBuffer() = default;

    /**
     * @brief Push an element to the back of the queue.
     * @param item Element to push (moved)
     * @return true if successful, false if buffer is full
     */
    bool push(T item);

    /**
     * @brief Pop an element from the front of the queue.
     * @return The element if available, nullopt if buffer is empty
     */
    std::optional<T> pop();

    /**
     * @brief Check if the buffer is full.
     */
    [[nodiscard]] bool full() const;

    /**
     * @brief Check if the buffer is empty.
     */
    [[nodiscard]] bool empty() const;

    /**
     * @brief Number of elements currently in the buffer.
     */
    [[nodiscard]] std::size_t size() const;

    /**
     * @brief Buffer capacity (maximum elements).
     */
    [[nodiscard]] std::size_t capacity() const;

    /**
     * @brief Clear the buffer (remove all elements).
     */
    void clear();

private:
    std::vector<std::optional<T>> m_buffer;
    std::size_t                   m_read_pos  = 0;
    std::size_t                   m_write_pos = 0;
    std::size_t                   m_size      = 0;
};

// ---------------------------------------------------------------------------
// Template Implementation (inline)
// ---------------------------------------------------------------------------

template<typename T>
RingBuffer<T>::RingBuffer(std::size_t capacity)
    : m_buffer(capacity)
    , m_read_pos(0)
    , m_write_pos(0)
    , m_size(0)
{
}

template<typename T>
bool RingBuffer<T>::push(T item) {
    if (m_size == m_buffer.size()) {
        return false; // Buffer full
    }

    m_buffer[m_write_pos] = std::move(item);
    m_write_pos = (m_write_pos + 1) % m_buffer.size();
    ++m_size;
    return true;
}

template<typename T>
std::optional<T> RingBuffer<T>::pop() {
    if (m_size == 0) {
        return std::nullopt; // Buffer empty
    }

    std::optional<T> result = std::move(m_buffer[m_read_pos]);
    m_buffer[m_read_pos] = std::nullopt; // Clear the slot
    m_read_pos = (m_read_pos + 1) % m_buffer.size();
    --m_size;
    return result;
}

template<typename T>
bool RingBuffer<T>::full() const {
    return m_size == m_buffer.size();
}

template<typename T>
bool RingBuffer<T>::empty() const {
    return m_size == 0;
}

template<typename T>
std::size_t RingBuffer<T>::size() const {
    return m_size;
}

template<typename T>
std::size_t RingBuffer<T>::capacity() const {
    return m_buffer.size();
}

template<typename T>
void RingBuffer<T>::clear() {
    for (auto& slot : m_buffer) {
        slot = std::nullopt;
    }
    m_read_pos = 0;
    m_write_pos = 0;
    m_size = 0;
}

} // namespace coro
