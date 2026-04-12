#pragma once

#include <coro/io/decoder_concept.h>
#include "pcie_packet.h"
#include <cstddef>
#include <optional>
#include <span>

namespace coro {

/**
 * @brief Stateful decoder for PCIe character device multi-stage packet protocol.
 *
 * Decodes packets with the following structure:
 * - Header1 (16 bytes) - contains flags and payload length
 * - Header2 (16 bytes, optional) - present if header1.flags & 0x01
 * - Payload (variable, 4KB-128KB) - size from header1
 * - Footer (16 bytes) - magic pattern for frame sync validation
 *
 * The decoder maintains state across multiple decode() calls to handle
 * packets that span multiple read() operations.
 *
 * Supports zero-copy: payload can be read directly into the packet's
 * pre-allocated vector to avoid memcpy.
 */
class PcieDecoder {
public:
    using OutputType = PciePacket;

    PcieDecoder() = default;
    ~PcieDecoder() = default;

    PcieDecoder(PcieDecoder&&) noexcept = default;
    PcieDecoder& operator=(PcieDecoder&&) noexcept = default;
    PcieDecoder(const PcieDecoder&) = delete;
    PcieDecoder& operator=(const PcieDecoder&) = delete;

    /**
     * @brief Decode one packet from the byte buffer.
     *
     * @param buffer Raw bytes to decode from
     * @param consumed Output: number of bytes consumed from buffer
     * @return some(PciePacket) if complete packet decoded, nullopt if need more bytes
     * @throws std::runtime_error on framing error (invalid footer magic, invalid payload length)
     */
    std::optional<PciePacket> decode(std::span<const std::byte> buffer,
                                     std::size_t& consumed);

    /**
     * @brief Reset decoder to initial state.
     *
     * Used for error recovery after framing error.
     */
    void reset();

    // -----------------------------------------------------------------------
    // Zero-copy payload support
    // -----------------------------------------------------------------------

    /**
     * @brief Check if decoder is in payload-reading state.
     *
     * When true, payload can be read directly into get_payload_write_target().
     */
    [[nodiscard]] bool is_reading_payload() const;

    /**
     * @brief Get writable span into work-in-progress packet's payload.
     *
     * Returns span starting at current payload write position.
     * Only valid when is_reading_payload() returns true.
     */
    [[nodiscard]] std::span<std::byte> get_payload_write_target();

    /**
     * @brief Notify decoder that n bytes were written directly to payload.
     *
     * @param n Number of bytes written (must be <= get_payload_write_target().size())
     */
    void commit_payload_bytes(std::size_t n);

    /**
     * @brief Check if payload is complete (all expected bytes received).
     */
    [[nodiscard]] bool payload_complete() const;

private:
    enum class State {
        ReadingHeader1,   // Need 16 bytes
        ReadingHeader2,   // Need 16 bytes (conditional)
        ReadingPayload,   // Need N bytes (from header1.payload_length)
        ReadingFooter,    // Need 16 bytes
    };

    State       m_state = State::ReadingHeader1;
    PciePacket  m_packet;                  // Work-in-progress packet
    std::size_t m_payload_bytes_read = 0;  // Payload progress (for zero-copy)
    bool        m_has_header2 = false;     // Cached from header1.flags
};

// Verify PcieDecoder satisfies the ZeroCopyDecoder concept
static_assert(ZeroCopyDecoder<PcieDecoder>);

} // namespace coro
