#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/io/circular_byte_buffer.h>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <exception>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace coro {

// ---------------------------------------------------------------------------
// Tag and concept identifying futures produced by ByteSource
// ---------------------------------------------------------------------------

/// @brief Tag base class inherited by all futures returned by ByteSource.
/// Used by DecoderCoroPromise::await_transform to restrict co_await to
/// ByteSource futures only.
struct ByteSourceFutureTag {};

/// @brief Concept satisfied by MemcpyFuture, ReadFuture, and any future
/// ByteSource-produced future types.
template<typename F>
concept ByteSourceFuture =
    std::is_base_of_v<ByteSourceFutureTag, std::remove_cvref_t<F>>;

// ---------------------------------------------------------------------------
// ByteSource
// ---------------------------------------------------------------------------

/**
 * @brief Byte delivery interface for DecoderStream coroutines.
 *
 * Owned by PollStream::State and lives entirely on the libuv I/O thread.
 * poll_cb writes raw bytes from the fd into the accumulation ring buffer;
 * the decoder coroutine reads them via memcpy() and read().
 *
 * All methods are libuv-thread-only. No synchronisation is needed.
 *
 * @see MemcpyFuture  For fixed-size struct reads (headers, footers).
 * @see ReadFuture    For variable-length or large reads where the caller
 *                    manages the destination buffer.
 */
class ByteSource {
public:
    class MemcpyFuture;
    class ReadFuture;

    explicit ByteSource(std::size_t capacity) : m_buffer(capacity) {}

    ByteSource(ByteSource&&) noexcept = default;
    ByteSource(const ByteSource&)     = delete;
    ByteSource& operator=(const ByteSource&) = delete;
    ByteSource& operator=(ByteSource&&)      = delete;

    // ------------------------------------------------------------------
    // Decoder-facing API
    // ------------------------------------------------------------------

    /// Copies exactly n bytes into dst. Returns n on success.
    /// Returns 0 on clean EOF before the first byte (decoder should co_return).
    /// Throws on EOF after at least one byte (unexpected truncation).
    [[nodiscard]] MemcpyFuture memcpy(void* dst, std::size_t n);

    /// Returns up to max_bytes contiguous bytes — read(2) semantics.
    /// May return fewer bytes at a ring-buffer wrap boundary.
    /// Returns an empty span on clean EOF. Call consume(n) after use.
    [[nodiscard]] ReadFuture read(std::size_t max_bytes);

    /// Advance the read pointer by n bytes.
    void consume(std::size_t n) { m_buffer.consume(n); }

    // ------------------------------------------------------------------
    // poll_cb-facing API (libuv thread only)
    // ------------------------------------------------------------------

    std::span<std::byte> writable_span()        { return m_buffer.writable_span(); }
    void   commit_write(std::size_t n)           { m_buffer.commit_write(n); }
    std::size_t writable_bytes() const           { return m_buffer.writable_bytes(); }
    std::size_t readable_bytes() const           { return m_buffer.readable_bytes(); }

    void set_eof()                               { m_eof = true; }
    void set_error(std::exception_ptr e)         { m_error = std::move(e); }
    bool fd_eof()   const noexcept               { return m_eof; }
    bool has_error() const noexcept              { return m_error != nullptr; }

private:
    friend class MemcpyFuture;
    friend class ReadFuture;

    CircularByteBuffer m_buffer;
    bool               m_eof   = false;
    std::exception_ptr m_error;
};

// ---------------------------------------------------------------------------
// MemcpyFuture
// ---------------------------------------------------------------------------

/**
 * @brief Future that copies exactly n bytes from the ByteSource accumulation
 * buffer into a caller-supplied destination, looping across ring-buffer wrap
 * boundaries internally.
 *
 * Return value (OutputType = std::size_t):
 * - **n**  — all n bytes were successfully copied.
 * - **0**  — EOF was seen before the first byte: clean packet-boundary EOF.
 *            The decoder should call `co_return` when it observes this.
 * - **PollError** — EOF arrived after at least one byte (unexpected truncation),
 *            or an I/O error was signalled on the fd.
 *
 * Obtained via ByteSource::memcpy(). Do not construct directly.
 */
class ByteSource::MemcpyFuture : public ByteSourceFutureTag {
public:
    using OutputType = std::size_t;

    MemcpyFuture(ByteSource& src, void* dst, std::size_t n)
        : m_src(&src), m_dst(static_cast<std::byte*>(dst)), m_n(n) {}

    MemcpyFuture(MemcpyFuture&&) noexcept = default;

    PollResult<std::size_t> poll(detail::Context&) {
        while (m_copied < m_n) {
            if (m_src->m_error)
                return PollError(m_src->m_error);
            auto chunk = m_src->m_buffer.readable_span();
            if (chunk.empty()) {
                if (m_src->m_eof) {
                    if (m_copied == 0)
                        return std::size_t{0}; // clean EOF between packets
                    return PollError(std::make_exception_ptr(
                        std::runtime_error("unexpected EOF mid-read")));
                }
                return PollPending;
            }
            std::size_t take = std::min(chunk.size(), m_n - m_copied);
            std::memcpy(m_dst + m_copied, chunk.data(), take);
            m_src->m_buffer.consume(take);
            m_copied += take;
        }
        return m_n;
    }

private:
    ByteSource*  m_src;
    std::byte*   m_dst;
    std::size_t  m_n;
    std::size_t  m_copied = 0;
};

// ---------------------------------------------------------------------------
// ReadFuture
// ---------------------------------------------------------------------------

/**
 * @brief Future that returns up to max_bytes contiguous bytes from the
 * ByteSource accumulation buffer — identical semantics to a non-blocking
 * read(2) syscall.
 *
 * - Resolves with a non-empty span of [1, max_bytes] bytes when data is
 *   available. May return fewer bytes than max_bytes at a ring-buffer wrap
 *   boundary; the caller must loop as with a raw read(2).
 * - Resolves with an **empty span** on clean EOF. The decoder should
 *   call `co_return` when it observes this.
 * - Returns PollError on an I/O error.
 *
 * The returned span is valid until the next co_await on this ByteSource.
 * Call ByteSource::consume(n) after processing the bytes.
 *
 * Obtained via ByteSource::read(). Do not construct directly.
 */
class ByteSource::ReadFuture : public ByteSourceFutureTag {
public:
    using OutputType = std::span<const std::byte>;

    ReadFuture(ByteSource& src, std::size_t max_bytes)
        : m_src(&src), m_max(max_bytes) {}

    ReadFuture(ReadFuture&&) noexcept = default;

    PollResult<std::span<const std::byte>> poll(detail::Context&) {
        if (m_src->m_error)
            return PollError(m_src->m_error);
        auto chunk = m_src->m_buffer.readable_span();
        if (chunk.empty()) {
            if (m_src->m_eof)
                return std::span<const std::byte>{};
            return PollPending;
        }
        return chunk.subspan(0, std::min(chunk.size(), m_max));
    }

private:
    ByteSource*  m_src;
    std::size_t  m_max;
};

// ---------------------------------------------------------------------------
// Inline definitions
// ---------------------------------------------------------------------------

inline ByteSource::MemcpyFuture ByteSource::memcpy(void* dst, std::size_t n) {
    return MemcpyFuture{*this, dst, n};
}

inline ByteSource::ReadFuture ByteSource::read(std::size_t max_bytes) {
    return ReadFuture{*this, max_bytes};
}

} // namespace coro
