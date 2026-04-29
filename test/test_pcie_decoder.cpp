#include <gtest/gtest.h>
#include "pcie_decoder.h"
#include "pcie_packet.h"
#include <array>
#include <cstring>
#include <vector>

using namespace coro;

// ---------------------------------------------------------------------------
// Helper: Build test packet bytes
// ---------------------------------------------------------------------------

class TestPacketBuilder {
public:
    TestPacketBuilder& with_header1(uint32_t magic, bool has_header2, uint32_t packet_type,
                                    uint32_t payload_len, uint64_t timestamp) {
        PciePacket::Header1 h1{magic, has_header2 ? 1u : 0u, packet_type, payload_len, timestamp};
        auto* ptr = reinterpret_cast<const std::byte*>(&h1);
        m_bytes.insert(m_bytes.end(), ptr, ptr + sizeof(h1));
        return *this;
    }

    TestPacketBuilder& with_header2(uint64_t sequence, uint64_t reserved) {
        PciePacket::Header2 h2{sequence, reserved};
        auto* ptr = reinterpret_cast<const std::byte*>(&h2);
        m_bytes.insert(m_bytes.end(), ptr, ptr + sizeof(h2));
        return *this;
    }

    TestPacketBuilder& with_payload(std::span<const std::byte> data) {
        m_bytes.insert(m_bytes.end(), data.begin(), data.end());
        return *this;
    }

    TestPacketBuilder& with_payload_size(std::size_t size, std::byte fill = std::byte{0}) {
        m_bytes.insert(m_bytes.end(), size, fill);
        return *this;
    }

    TestPacketBuilder& with_footer(uint64_t magic1, uint64_t magic2) {
        PciePacket::Footer footer{magic1, magic2};
        auto* ptr = reinterpret_cast<const std::byte*>(&footer);
        m_bytes.insert(m_bytes.end(), ptr, ptr + sizeof(footer));
        return *this;
    }

    std::vector<std::byte> build() const {
        return m_bytes;
    }

private:
    std::vector<std::byte> m_bytes;
};

// ---------------------------------------------------------------------------
// Decoder concept checks
// ---------------------------------------------------------------------------

static_assert(Decoder<PcieDecoder>);
static_assert(ZeroCopyDecoder<PcieDecoder>);

// ---------------------------------------------------------------------------
// Single packet decode (all data available)
// ---------------------------------------------------------------------------

TEST(PcieDecoderTest, DecodeMinimalPacket) {
    // Build packet with header1 only (no header2) + 4KB payload + footer
    auto packet_bytes = TestPacketBuilder()
        .with_header1(0x12345678, false, 0, 4096, 0xABCDEF0123456789ULL)
        .with_payload_size(4096, std::byte{0xAA})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    PcieDecoder decoder;
    std::size_t consumed = 0;
    auto result = decoder.decode(packet_bytes, consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(consumed, packet_bytes.size());

    auto& pkt = *result;
    EXPECT_EQ(pkt.header1.magic, 0x12345678U);
    EXPECT_EQ(pkt.header1.has_header2, 0u);
    EXPECT_EQ(pkt.header1.payload_length, 4096u);
    EXPECT_EQ(pkt.header1.timestamp, 0xABCDEF0123456789ULL);
    EXPECT_FALSE(pkt.header2.has_value());
    EXPECT_EQ(pkt.payload.size(), 4096);
    EXPECT_EQ(pkt.payload[0], std::byte{0xAA});
    EXPECT_EQ(pkt.footer.magic1, PCIE_FOOTER_MAGIC1);
    EXPECT_EQ(pkt.footer.magic2, PCIE_FOOTER_MAGIC2);
}

TEST(PcieDecoderTest, DecodePacketWithHeader2) {
    // Build packet with header1 (flag set) + header2 + payload + footer
    auto packet_bytes = TestPacketBuilder()
        .with_header1(0x87654321, true, 0, 8192, 0x1122334455667788ULL)
        .with_header2(0x9999999999999999ULL, 0x0000000000000000ULL)
        .with_payload_size(8192, std::byte{0xBB})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    PcieDecoder decoder;
    std::size_t consumed = 0;
    auto result = decoder.decode(packet_bytes, consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(consumed, packet_bytes.size());

    auto& pkt = *result;
    EXPECT_EQ(pkt.header1.has_header2, 1u);
    ASSERT_TRUE(pkt.header2.has_value());
    EXPECT_EQ(pkt.header2->sequence_number, 0x9999999999999999ULL);
    EXPECT_EQ(pkt.payload.size(), 8192);
}

TEST(PcieDecoderTest, DISABLED_DecodeMaxSizePayload) {
    // Build packet with 128KB payload (maximum allowed)
    auto packet_bytes = TestPacketBuilder()
        .with_header1(0x11111111, false, 0, 128 * 1024, 0x1234567890ABCDEFULL)
        .with_payload_size(128 * 1024, std::byte{0xCC})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    PcieDecoder decoder;
    std::size_t consumed = 0;
    auto result = decoder.decode(packet_bytes, consumed);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload.size(), 128 * 1024);
}

// ---------------------------------------------------------------------------
// Multi-stage decode (partial data)
// ---------------------------------------------------------------------------

TEST(PcieDecoderTest, DISABLED_DecodeAcrossMultipleCalls_Header1Split) {
    // Build complete packet
    auto packet_bytes = TestPacketBuilder()
        .with_header1(0xAABBCCDD, false, 0, 4096, 0x1111111111111111ULL)
        .with_payload_size(4096, std::byte{0xDD})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    PcieDecoder decoder;

    // Feed first 8 bytes of header1
    std::span<const std::byte> chunk1(packet_bytes.data(), 8);
    std::size_t consumed1 = 0;
    auto result1 = decoder.decode(chunk1, consumed1);
    EXPECT_FALSE(result1.has_value());  // Need more bytes
    EXPECT_EQ(consumed1, 0);  // Nothing consumed yet

    // Feed remaining bytes
    std::span<const std::byte> chunk2(packet_bytes.data() + 8, packet_bytes.size() - 8);
    std::size_t consumed2 = 0;
    auto result2 = decoder.decode(chunk2, consumed2);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(consumed2, packet_bytes.size() - 8);
}

TEST(PcieDecoderTest, DISABLED_DecodeAcrossMultipleCalls_PayloadSplit) {
    // Build complete packet
    auto packet_bytes = TestPacketBuilder()
        .with_header1(0x55555555, false, 0, 4096, 0x7777ULL)
        .with_payload_size(4096, std::byte{0x55})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    PcieDecoder decoder;

    // Feed header first (16 bytes)
    std::span<const std::byte> header(packet_bytes.data(), 16);
    std::size_t consumed_h = 0;
    auto result_h = decoder.decode(header, consumed_h);
    EXPECT_EQ(consumed_h, 16);
    EXPECT_FALSE(result_h.has_value());  // Need payload

    // Feed half payload (2KB)
    std::span<const std::byte> payload1(packet_bytes.data() + 16, 2048);
    std::size_t consumed1 = 0;
    auto result1 = decoder.decode(payload1, consumed1);
    EXPECT_EQ(consumed1, 2048);
    EXPECT_FALSE(result1.has_value());  // Need rest of payload

    // Feed rest (2KB payload + 16 byte footer)
    std::span<const std::byte> rest(packet_bytes.data() + 16 + 2048, 2048 + 16);
    std::size_t consumed2 = 0;
    auto result2 = decoder.decode(rest, consumed2);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2->header1.magic, 0x55555555U);
}

TEST(PcieDecoderTest, DecodeAcrossMultipleCalls_FooterSplit) {
    // Build complete packet
    auto packet_bytes = TestPacketBuilder()
        .with_header1(0x66666666, false, 0, 4096, 0x8888ULL)
        .with_payload_size(4096, std::byte{0x66})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    PcieDecoder decoder;

    // Feed everything except footer
    std::size_t without_footer = packet_bytes.size() - 16;
    std::span<const std::byte> almost_all(packet_bytes.data(), without_footer);
    std::size_t consumed1 = 0;
    auto result1 = decoder.decode(almost_all, consumed1);
    EXPECT_FALSE(result1.has_value());  // Need footer

    // Feed footer
    std::span<const std::byte> footer_only(packet_bytes.data() + without_footer, 16);
    std::size_t consumed2 = 0;
    auto result2 = decoder.decode(footer_only, consumed2);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2->header1.magic, 0x66666666U);
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

TEST(PcieDecoderTest, InvalidPayloadLength_TooSmall) {
    // Build packet with payload_length = 1024 (< 4KB min)
    auto packet_bytes = TestPacketBuilder()
        .with_header1(0xFFFFFFFF, false, 0, 1024, 0x0ULL)  // 1024 < 4096 min
        .with_payload_size(1024)
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    PcieDecoder decoder;
    std::size_t consumed = 0;
    EXPECT_THROW(decoder.decode(packet_bytes, consumed), std::runtime_error);
}

TEST(PcieDecoderTest, InvalidPayloadLength_TooLarge) {
    // Build packet with payload_length = 256KB (> 128KB max)
    auto packet_bytes = TestPacketBuilder()
        .with_header1(0xFFFFFFFF, false, 0, 256 * 1024, 0x0ULL)  // 256KB > 128KB max
        .with_payload_size(256 * 1024)
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    PcieDecoder decoder;
    std::size_t consumed = 0;
    EXPECT_THROW(decoder.decode(packet_bytes, consumed), std::runtime_error);
}

TEST(PcieDecoderTest, InvalidFooterMagic) {
    // Build packet with wrong footer magic numbers
    auto packet_bytes = TestPacketBuilder()
        .with_header1(0x12345678, false, 0, 4096, 0x0ULL)
        .with_payload_size(4096)
        .with_footer(0xBADBADBADBADBAD, 0xBADBADBADBADBAD)  // Wrong magic
        .build();

    PcieDecoder decoder;
    std::size_t consumed = 0;
    EXPECT_THROW(decoder.decode(packet_bytes, consumed), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Zero-copy interface
// ---------------------------------------------------------------------------

TEST(PcieDecoderTest, ZeroCopy_DirectPayloadWrite) {
    // Build just header and footer (we'll write payload directly)
    auto header_bytes = TestPacketBuilder()
        .with_header1(0x99999999, false, 0, 4096, 0xAAAULL)
        .build();

    auto footer_bytes = TestPacketBuilder()
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    PcieDecoder decoder;

    // Decode header1 (decoder pre-allocates payload)
    std::size_t consumed_h = 0;
    auto result_h = decoder.decode(header_bytes, consumed_h);
    EXPECT_FALSE(result_h.has_value());

    // Verify decoder is in payload-reading state
    EXPECT_TRUE(decoder.is_reading_payload());
    EXPECT_FALSE(decoder.payload_complete());

    // Write payload directly via zero-copy interface
    auto write_target = decoder.get_payload_write_target();
    EXPECT_EQ(write_target.size(), 4096);
    std::fill(write_target.begin(), write_target.end(), std::byte{0x99});
    decoder.commit_payload_bytes(4096);

    EXPECT_TRUE(decoder.payload_complete());

    // Decode footer
    std::size_t consumed_f = 0;
    auto result = decoder.decode(footer_bytes, consumed_f);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->header1.magic, 0x99999999U);
    EXPECT_EQ(result->payload[0], std::byte{0x99});
}

TEST(PcieDecoderTest, ZeroCopy_PartialPayloadWrites) {
    // Build header and footer
    auto header_bytes = TestPacketBuilder()
        .with_header1(0xCCCCCCCC, false, 0, 4096, 0xBBBULL)
        .build();

    auto footer_bytes = TestPacketBuilder()
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    PcieDecoder decoder;

    // Decode header
    std::size_t consumed_h = 0;
    decoder.decode(header_bytes, consumed_h);

    // Write payload in 4 × 1KB chunks
    for (int i = 0; i < 4; ++i) {
        EXPECT_FALSE(decoder.payload_complete());
        auto write_target = decoder.get_payload_write_target();
        EXPECT_EQ(write_target.size(), 4096 - i * 1024);

        std::fill_n(write_target.begin(), 1024, std::byte{static_cast<uint8_t>(i)});
        decoder.commit_payload_bytes(1024);
    }

    EXPECT_TRUE(decoder.payload_complete());

    // Decode footer and verify
    std::size_t consumed_f = 0;
    auto result = decoder.decode(footer_bytes, consumed_f);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload[0], std::byte{0});
    EXPECT_EQ(result->payload[1024], std::byte{1});
    EXPECT_EQ(result->payload[2048], std::byte{2});
    EXPECT_EQ(result->payload[3072], std::byte{3});
}

// ---------------------------------------------------------------------------
// Multiple packets
// ---------------------------------------------------------------------------

TEST(PcieDecoderTest, DecodeMultiplePacketsSequentially) {
    // Build buffer with 3 complete packets back-to-back
    auto pkt1 = TestPacketBuilder()
        .with_header1(0x11111111, false, 0, 4096, 0x1ULL)
        .with_payload_size(4096, std::byte{0x11})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    auto pkt2 = TestPacketBuilder()
        .with_header1(0x22222222, false, 0, 4096, 0x2ULL)
        .with_payload_size(4096, std::byte{0x22})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    auto pkt3 = TestPacketBuilder()
        .with_header1(0x33333333, false, 0, 4096, 0x3ULL)
        .with_payload_size(4096, std::byte{0x33})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    // Concatenate all packets
    std::vector<std::byte> all_bytes;
    all_bytes.insert(all_bytes.end(), pkt1.begin(), pkt1.end());
    all_bytes.insert(all_bytes.end(), pkt2.begin(), pkt2.end());
    all_bytes.insert(all_bytes.end(), pkt3.begin(), pkt3.end());

    PcieDecoder decoder;
    std::span<const std::byte> buffer = all_bytes;

    // Decode 1st packet
    std::size_t consumed1 = 0;
    auto result1 = decoder.decode(buffer, consumed1);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1->header1.magic, 0x11111111U);
    EXPECT_EQ(result1->payload[0], std::byte{0x11});
    buffer = buffer.subspan(consumed1);

    // Decode 2nd packet
    std::size_t consumed2 = 0;
    auto result2 = decoder.decode(buffer, consumed2);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2->header1.magic, 0x22222222U);
    EXPECT_EQ(result2->payload[0], std::byte{0x22});
    buffer = buffer.subspan(consumed2);

    // Decode 3rd packet
    std::size_t consumed3 = 0;
    auto result3 = decoder.decode(buffer, consumed3);
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(result3->header1.magic, 0x33333333U);
    EXPECT_EQ(result3->payload[0], std::byte{0x33});
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST(PcieDecoderTest, ResetAfterPartialDecode) {
    // Build complete packet
    auto packet_bytes = TestPacketBuilder()
        .with_header1(0xDEADBEEF, false, 0, 4096, 0x9999ULL)
        .with_payload_size(4096, std::byte{0xEE})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();

    PcieDecoder decoder;

    // Start decoding (feed partial data - just header1)
    std::span<const std::byte> partial(packet_bytes.data(), 16);
    std::size_t consumed1 = 0;
    auto result1 = decoder.decode(partial, consumed1);
    EXPECT_FALSE(result1.has_value());  // Still need more data

    // Reset decoder
    decoder.reset();

    // Now decode a fresh packet successfully
    std::size_t consumed2 = 0;
    auto result2 = decoder.decode(packet_bytes, consumed2);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2->header1.magic, 0xDEADBEEF);
}
