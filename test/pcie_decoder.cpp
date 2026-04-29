#include "pcie_decoder.h"
#include <cassert>
#include <cstring>
#include <span>
#include <stdexcept>

namespace coro {

std::optional<PciePacket> PcieDecoder::decode(std::span<const std::byte> buffer,
                                               std::size_t& consumed) {
    consumed = 0;

    while (true) {
        switch (m_state) {
        case State::ReadingHeader1: {
            if (buffer.size() < 16) {
                return std::nullopt; // Need more bytes
            }

            // Parse primary header (16 bytes)
            std::memcpy(&m_packet.header1, buffer.data(), 16);
            buffer = buffer.subspan(16);
            consumed += 16;

            // Extract flags and payload length
            m_has_header2 = (m_packet.header1.has_header2) != 0;
            std::size_t payload_length = m_packet.header1.payload_length;

            // Validate payload length
            if (payload_length < PCIE_MIN_PAYLOAD_SIZE || payload_length > PCIE_MAX_PAYLOAD_SIZE) {
                throw std::runtime_error(
                    "Invalid payload length: " + std::to_string(payload_length) +
                    " (must be between " + std::to_string(PCIE_MIN_PAYLOAD_SIZE) +
                    " and " + std::to_string(PCIE_MAX_PAYLOAD_SIZE) + ")"
                );
            }

            // PRE-ALLOCATE payload vector for zero-copy writes
            m_packet.payload.resize(payload_length);
            m_payload_bytes_read = 0;

            // Transition to next state
            m_state = m_has_header2 ? State::ReadingHeader2 : State::ReadingPayload;
            continue; // Try next state immediately
        }

        case State::ReadingHeader2: {
            if (buffer.size() < 16) {
                return std::nullopt; // Need more bytes
            }

            // Parse secondary header (16 bytes)
            PciePacket::Header2 header2;
            std::memcpy(&header2, buffer.data(), 16);
            m_packet.header2 = header2;

            buffer = buffer.subspan(16);
            consumed += 16;

            m_state = State::ReadingPayload;
            continue;
        }

        case State::ReadingPayload: {
            // NOTE: This path is only used if poll_cb didn't read directly via zero-copy
            // If payload data was already in byte_buffer from previous read, copy it here
            std::size_t remaining = m_packet.payload.size() - m_payload_bytes_read;

            if (buffer.size() < remaining) {
                return std::nullopt; // Need more bytes
            }

            // Copy remaining payload bytes
            std::memcpy(
                m_packet.payload.data() + m_payload_bytes_read,
                buffer.data(),
                remaining
            );
            buffer = buffer.subspan(remaining);
            consumed += remaining;
            m_payload_bytes_read = m_packet.payload.size();

            m_state = State::ReadingFooter;
            continue;
        }

        case State::ReadingFooter: {
            if (buffer.size() < 16) {
                return std::nullopt; // Need more bytes
            }

            // Parse footer (16 bytes)
            std::memcpy(&m_packet.footer, buffer.data(), 16);
            consumed += 16;

            // Validate footer magic numbers
            if (m_packet.footer.magic1 != PCIE_FOOTER_MAGIC1 ||
                m_packet.footer.magic2 != PCIE_FOOTER_MAGIC2) {
                throw std::runtime_error(
                    "Invalid footer magic - frame sync lost. Expected: 0x" +
                    std::to_string(PCIE_FOOTER_MAGIC1) + " 0x" +
                    std::to_string(PCIE_FOOTER_MAGIC2)
                );
            }

            // Packet complete - reset state and return
            m_state = State::ReadingHeader1;
            auto completed_packet = std::move(m_packet);
            m_packet = PciePacket{}; // Reset for next packet
            m_payload_bytes_read = 0;
            m_has_header2 = false;
            return completed_packet;
        }

        default:
            // Should never happen
            throw std::runtime_error("PcieDecoder: Invalid state");
        }
    }
}

void PcieDecoder::reset() {
    m_state = State::ReadingHeader1;
    m_packet = PciePacket{};
    m_payload_bytes_read = 0;
    m_has_header2 = false;
}

bool PcieDecoder::is_reading_payload() const {
    return m_state == State::ReadingPayload;
}

std::span<std::byte> PcieDecoder::get_payload_write_target() {
    assert(m_state == State::ReadingPayload);
    assert(m_payload_bytes_read < m_packet.payload.size());

    return std::span<std::byte>(
        m_packet.payload.data() + m_payload_bytes_read,
        m_packet.payload.size() - m_payload_bytes_read
    );
}

void PcieDecoder::commit_payload_bytes(std::size_t n) {
    assert(m_state == State::ReadingPayload);
    assert(m_payload_bytes_read + n <= m_packet.payload.size());

    m_payload_bytes_read += n;
}

bool PcieDecoder::payload_complete() const {
    return m_payload_bytes_read >= m_packet.payload.size();
}

} // namespace coro
