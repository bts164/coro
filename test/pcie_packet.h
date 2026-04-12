#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace coro {

/**
 * @brief PCIe character device packet structure with multi-stage framing.
 *
 * Packet format:
 * 1. Header1 (16 bytes, fixed) - contains flags, payload length, timestamp
 * 2. Header2 (16 bytes, conditional) - present if flags & 0x01
 * 3. Payload (variable, 4KB-128KB) - size from header1.payload_length
 * 4. Footer (16 bytes, fixed magic pattern) - frame sync validation
 */
struct PciePacket {
    /**
     * @brief Primary header (always present, 16 bytes).
     */
    struct Header1 {
        uint32_t magic;           // Protocol magic number
        uint16_t flags;           // Bit 0: has_header2, bits 8-15: packet type
        uint16_t payload_length;  // Payload size in bytes (4096-131072)
        uint64_t timestamp;       // PCIe device timestamp
    } header1;

    /**
     * @brief Optional secondary header (16 bytes, present if header1.flags & 0x01).
     */
    struct Header2 {
        uint64_t sequence_number;
        uint64_t reserved;
    };
    std::optional<Header2> header2;

    /**
     * @brief Variable-length payload (4KB-128KB).
     *
     * Pre-allocated by decoder during header1 parsing to enable zero-copy reads.
     */
    std::vector<std::byte> payload;

    /**
     * @brief Footer with fixed magic pattern (16 bytes).
     */
    struct Footer {
        uint64_t magic1;  // Expected: 0xDEADBEEFCAFEBABE
        uint64_t magic2;  // Expected: 0x0123456789ABCDEF
    } footer;
};

// Footer magic constants for validation
inline constexpr uint64_t PCIE_FOOTER_MAGIC1 = 0xDEADBEEFCAFEBABEULL;
inline constexpr uint64_t PCIE_FOOTER_MAGIC2 = 0x0123456789ABCDEFULL;

// Payload size limits
inline constexpr std::size_t PCIE_MIN_PAYLOAD_SIZE = 4096;
inline constexpr std::size_t PCIE_MAX_PAYLOAD_SIZE = 128 * 1024;

} // namespace coro
