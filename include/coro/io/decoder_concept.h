#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>

namespace coro {

/**
 * @brief Concept for frame decoders used with PollStream.
 *
 * A Decoder extracts structured frames from a raw byte stream. It maintains
 * internal state across multiple decode() calls to handle frames that span
 * multiple read() operations.
 *
 * @tparam T The decoder type
 *
 * Requirements:
 * - Must define OutputType — the type of decoded frames
 * - decode(buffer, consumed) — extracts one frame, returns nullopt if need more bytes
 * - reset() — resets decoder to initial state (for error recovery)
 *
 * IMPORTANT: The buffer passed to decode() contains ONLY unconsumed bytes, not the
 * entire packet from the beginning. The decoder must track its own state and consume
 * bytes incrementally. The 'consumed' parameter indicates bytes consumed from THIS
 * buffer (not cumulative). PollStream will consume these bytes immediately, even if
 * the packet is incomplete. This design correctly handles wrap-around in circular buffers.
 *
 * Zero-copy support (optional):
 * - is_reading_payload() — returns true if decoder is in payload-reading state
 * - get_payload_write_target() — returns writable span for direct reads
 * - commit_payload_bytes(n) — notifies decoder that n bytes were written
 * - payload_complete() — returns true if payload fully received
 */
template<typename T>
concept Decoder = requires(T decoder,
                          std::span<const std::byte> buffer,
                          std::size_t& consumed,
                          std::size_t n) {
    // Output type of decoded frames
    typename T::OutputType;

    // Extract one frame from buffer, updating consumed to bytes read FROM THIS CALL
    // Buffer contains ONLY unconsumed bytes (not from start of packet)
    // Returns:
    //   some(OutputType) - successfully decoded a complete frame
    //   nullopt          - need more bytes (partial frame)
    // Throws std::runtime_error on framing error (invalid data)
    // Note: consumed is set to bytes read from THIS buffer, not cumulative total
    { decoder.decode(buffer, consumed) } -> std::same_as<std::optional<typename T::OutputType>>;

    // Reset decoder to initial state (e.g., after error recovery)
    { decoder.reset() } -> std::same_as<void>;
};

/**
 * @brief Extended decoder concept with zero-copy payload support.
 *
 * Decoders implementing this concept can have large payloads read directly
 * into the work-in-progress packet, avoiding a memcpy from the byte buffer.
 */
template<typename T>
concept ZeroCopyDecoder = Decoder<T> && requires(T decoder, std::size_t n) {
    // Returns true if decoder is in payload-reading state
    { decoder.is_reading_payload() } -> std::same_as<bool>;

    // Get writable span into work-in-progress packet's payload
    { decoder.get_payload_write_target() } -> std::same_as<std::span<std::byte>>;

    // Notify decoder that n bytes were written directly to payload
    { decoder.commit_payload_bytes(n) } -> std::same_as<void>;

    // Returns true if payload is complete (all expected bytes received)
    { decoder.payload_complete() } -> std::same_as<bool>;
};

} // namespace coro
