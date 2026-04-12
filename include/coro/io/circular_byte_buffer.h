#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace coro {

/**
 * @brief Circular byte buffer for streaming I/O.
 *
 * Provides a fixed-capacity circular buffer for raw bytes. Used by PollStream
 * to buffer data between read() syscalls and frame decoding.
 *
 * Thread safety: Not thread-safe. Caller must ensure exclusive access.
 */
class CircularByteBuffer {
public:
    /**
     * @brief Construct a circular buffer with the given capacity.
     * @param capacity Buffer size in bytes
     */
    explicit CircularByteBuffer(std::size_t capacity);

    CircularByteBuffer(CircularByteBuffer&&) noexcept = default;
    CircularByteBuffer& operator=(CircularByteBuffer&&) noexcept = default;
    CircularByteBuffer(const CircularByteBuffer&) = delete;
    CircularByteBuffer& operator=(const CircularByteBuffer&) = delete;

    ~CircularByteBuffer() = default;

    /**
     * @brief Get a contiguous writable span.
     *
     * Returns a span where new data can be written. May be smaller than
     * writable_bytes() if the writable region wraps around the buffer end.
     *
     * After writing, call commit_write(n) to advance the write pointer.
     */
    [[nodiscard]] std::span<std::byte> writable_span();

    /**
     * @brief Commit n bytes that were written to writable_span().
     * @param n Number of bytes written (must be <= writable_span().size())
     */
    void commit_write(std::size_t n);

    /**
     * @brief Get a contiguous readable span.
     *
     * Returns a span of buffered data. May be smaller than readable_bytes()
     * if the readable region wraps around the buffer end.
     *
     * After reading, call consume(n) to advance the read pointer.
     */
    [[nodiscard]] std::span<const std::byte> readable_span() const;

    /**
     * @brief Consume n bytes from the read pointer.
     * @param n Number of bytes to consume (must be <= readable_span().size())
     */
    void consume(std::size_t n);

    /**
     * @brief Total bytes available for reading.
     */
    [[nodiscard]] std::size_t readable_bytes() const;

    /**
     * @brief Total bytes available for writing.
     */
    [[nodiscard]] std::size_t writable_bytes() const;

    /**
     * @brief Buffer capacity in bytes.
     */
    [[nodiscard]] std::size_t capacity() const;

    /**
     * @brief Clear the buffer (reset read/write pointers).
     */
    void clear();

private:
    std::vector<std::byte> m_buffer;
    std::size_t            m_read_pos  = 0;  // Next byte to read
    std::size_t            m_write_pos = 0;  // Next byte to write
    std::size_t            m_size      = 0;  // Bytes currently buffered
};

} // namespace coro
