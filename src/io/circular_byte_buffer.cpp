#include <coro/io/circular_byte_buffer.h>
#include <algorithm>
#include <cassert>

namespace coro {

CircularByteBuffer::CircularByteBuffer(std::size_t capacity)
    : m_buffer(capacity)
    , m_read_pos(0)
    , m_write_pos(0)
    , m_size(0)
{
}

std::span<std::byte> CircularByteBuffer::writable_span() {
    if (m_size == m_buffer.size()) {
        // Buffer full
        return {};
    }

    // Return contiguous writable region from write_pos
    // May be smaller than writable_bytes() if write would wrap
    std::size_t contiguous = std::min(
        m_buffer.size() - m_size,           // Available space
        m_buffer.size() - m_write_pos       // Space until end of buffer
    );

    return std::span<std::byte>(m_buffer.data() + m_write_pos, contiguous);
}

void CircularByteBuffer::commit_write(std::size_t n) {
    assert(n <= m_buffer.size() - m_size && "Cannot write more than available space");
    assert(n <= m_buffer.size() - m_write_pos && "Write would exceed buffer boundary");

    m_write_pos = (m_write_pos + n) % m_buffer.size();
    m_size += n;
}

std::span<const std::byte> CircularByteBuffer::readable_span() const {
    if (m_size == 0) {
        // Buffer empty
        return {};
    }

    // Return contiguous readable region from read_pos
    // May be smaller than readable_bytes() if read would wrap
    std::size_t contiguous = std::min(
        m_size,                         // Available data
        m_buffer.size() - m_read_pos    // Data until end of buffer
    );

    return std::span<const std::byte>(m_buffer.data() + m_read_pos, contiguous);
}

void CircularByteBuffer::consume(std::size_t n) {
    assert(n <= m_size && "Cannot consume more than available data");
    assert(n <= m_buffer.size() - m_read_pos && "Consume would exceed buffer boundary");

    m_read_pos = (m_read_pos + n) % m_buffer.size();
    m_size -= n;
}

std::size_t CircularByteBuffer::readable_bytes() const {
    return m_size;
}

std::size_t CircularByteBuffer::writable_bytes() const {
    return m_buffer.size() - m_size;
}

std::size_t CircularByteBuffer::capacity() const {
    return m_buffer.size();
}

void CircularByteBuffer::clear() {
    m_read_pos = 0;
    m_write_pos = 0;
    m_size = 0;
}

} // namespace coro
