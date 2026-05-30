#include <gtest/gtest.h>
#include "pcie_decoder.h"
#include "pcie_stream.h"
#include "pcie_packet.h"
#include <coro/io/poll_stream.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <optional>
#include <vector>

using namespace coro;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct PipePair {
    int read_fd  = -1;
    int write_fd = -1;

    PipePair() {
        int fds[2];
        if (pipe(fds) != 0) throw std::runtime_error("pipe() failed");
        read_fd  = fds[0];
        write_fd = fds[1];
        int flags = fcntl(read_fd, F_GETFL);
        fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);
    }

    ~PipePair() {
        if (read_fd  >= 0) ::close(read_fd);
        if (write_fd >= 0) ::close(write_fd);
    }

    void write_bytes(const void* data, std::size_t len) {
        const auto* p = static_cast<const char*>(data);
        std::size_t done = 0;
        while (done < len) {
            ssize_t n = ::write(write_fd, p + done, len - done);
            if (n > 0) done += static_cast<std::size_t>(n);
        }
    }

    void close_write_end() {
        if (write_fd >= 0) { ::close(write_fd); write_fd = -1; }
    }
};

class TestPacketBuilder {
public:
    TestPacketBuilder& with_header1(uint32_t magic, bool has_header2, uint32_t packet_type,
                                    uint32_t payload_len, uint64_t timestamp) {
        PciePacket::Header1 h1{magic, has_header2 ? 1u : 0u, packet_type, payload_len, timestamp};
        append(&h1, sizeof(h1));
        return *this;
    }

    TestPacketBuilder& with_header2(uint64_t sequence, uint64_t reserved) {
        PciePacket::Header2 h2{sequence, reserved};
        append(&h2, sizeof(h2));
        return *this;
    }

    TestPacketBuilder& with_payload_size(std::size_t size, std::byte fill = std::byte{0}) {
        m_bytes.insert(m_bytes.end(), size, fill);
        return *this;
    }

    TestPacketBuilder& with_footer(uint64_t magic1, uint64_t magic2) {
        PciePacket::Footer f{magic1, magic2};
        append(&f, sizeof(f));
        return *this;
    }

    std::vector<std::byte> build() const { return m_bytes; }

private:
    std::vector<std::byte> m_bytes;
    void append(const void* data, std::size_t n) {
        auto* ptr = static_cast<const std::byte*>(data);
        m_bytes.insert(m_bytes.end(), ptr, ptr + n);
    }
};

static_assert(Stream<PollStream<PciePacket>>);

// ---------------------------------------------------------------------------
// Single-packet tests
// ---------------------------------------------------------------------------

TEST(PcieDecoderTest, DecodeMinimalPacket) {
    PipePair pipe;
    auto bytes = TestPacketBuilder()
        .with_header1(0x12345678, false, 0, 4096, 0xABCDEF0123456789ULL)
        .with_payload_size(4096, std::byte{0xAA})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();
    pipe.write_bytes(bytes.data(), bytes.size());
    pipe.close_write_end();

    std::optional<PciePacket> received;
    Runtime rt(1);
    rt.block_on([](int fd, std::optional<PciePacket>& out) -> Coro<void> {
        auto stream = PollStream<PciePacket>::open(fd, pcie_decoder);
        out = co_await next(stream);
    }(pipe.read_fd, received));

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->header1.magic,          0x12345678U);
    EXPECT_EQ(received->header1.has_header2,    0u);
    EXPECT_EQ(received->header1.payload_length, 4096u);
    EXPECT_EQ(received->header1.timestamp,      0xABCDEF0123456789ULL);
    EXPECT_FALSE(received->header2.has_value());
    ASSERT_EQ(received->payload.size(),         4096u);
    EXPECT_EQ(received->payload[0],             std::byte{0xAA});
    EXPECT_EQ(received->footer.magic1,          PCIE_FOOTER_MAGIC1);
    EXPECT_EQ(received->footer.magic2,          PCIE_FOOTER_MAGIC2);
}

TEST(PcieDecoderTest, DecodePacketWithHeader2) {
    PipePair pipe;
    auto bytes = TestPacketBuilder()
        .with_header1(0x87654321, true, 0, 8192, 0x1122334455667788ULL)
        .with_header2(0x9999999999999999ULL, 0x0000000000000000ULL)
        .with_payload_size(8192, std::byte{0xBB})
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();
    pipe.write_bytes(bytes.data(), bytes.size());
    pipe.close_write_end();

    std::optional<PciePacket> received;
    Runtime rt(1);
    rt.block_on([](int fd, std::optional<PciePacket>& out) -> Coro<void> {
        auto stream = PollStream<PciePacket>::open(fd, pcie_decoder);
        out = co_await next(stream);
    }(pipe.read_fd, received));

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->header1.has_header2,      1u);
    ASSERT_TRUE(received->header2.has_value());
    EXPECT_EQ(received->header2->sequence_number, 0x9999999999999999ULL);
    ASSERT_EQ(received->payload.size(),           8192u);
}

TEST(PcieDecoderTest, DecodeMultiplePacketsSequentially) {
    PipePair pipe;
    for (uint32_t i = 1; i <= 3; i++) {
        auto pkt = TestPacketBuilder()
            .with_header1(i * 0x11111111, false, 0, 4096, static_cast<uint64_t>(i))
            .with_payload_size(4096, static_cast<std::byte>(i))
            .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
            .build();
        pipe.write_bytes(pkt.data(), pkt.size());
    }
    pipe.close_write_end();

    std::vector<uint32_t> magics;
    Runtime rt(1);
    rt.block_on([](int fd, std::vector<uint32_t>& out) -> Coro<void> {
        auto stream = PollStream<PciePacket>::open(fd, pcie_decoder);
        while (auto pkt = co_await next(stream))
            out.push_back(pkt->header1.magic);
    }(pipe.read_fd, magics));

    ASSERT_EQ(magics.size(), 3u);
    EXPECT_EQ(magics[0], 0x11111111U);
    EXPECT_EQ(magics[1], 0x22222222U);
    EXPECT_EQ(magics[2], 0x33333333U);
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

TEST(PcieDecoderTest, InvalidPayloadLength_TooSmall) {
    PipePair pipe;
    auto bytes = TestPacketBuilder()
        .with_header1(0xFFFFFFFF, false, 0, 1024, 0x0ULL)
        .with_payload_size(1024)
        .with_footer(PCIE_FOOTER_MAGIC1, PCIE_FOOTER_MAGIC2)
        .build();
    pipe.write_bytes(bytes.data(), bytes.size());
    pipe.close_write_end();

    std::exception_ptr caught;
    Runtime rt(1);
    rt.block_on([](int fd, std::exception_ptr& out) -> Coro<void> {
        auto stream = PollStream<PciePacket>::open(fd, pcie_decoder);
        try { co_await next(stream); }
        catch (...) { out = std::current_exception(); }
    }(pipe.read_fd, caught));

    ASSERT_NE(caught, nullptr);
    EXPECT_THROW(std::rethrow_exception(caught), std::runtime_error);
}

TEST(PcieDecoderTest, InvalidFooterMagic) {
    PipePair pipe;
    auto bytes = TestPacketBuilder()
        .with_header1(0x12345678, false, 0, 4096, 0x0ULL)
        .with_payload_size(4096)
        .with_footer(0xBADBADBADBADBADULL, 0xBADBADBADBADBADULL)
        .build();
    pipe.write_bytes(bytes.data(), bytes.size());
    pipe.close_write_end();

    std::exception_ptr caught;
    Runtime rt(1);
    rt.block_on([](int fd, std::exception_ptr& out) -> Coro<void> {
        auto stream = PollStream<PciePacket>::open(fd, pcie_decoder);
        try { co_await next(stream); }
        catch (...) { out = std::current_exception(); }
    }(pipe.read_fd, caught));

    ASSERT_NE(caught, nullptr);
    EXPECT_THROW(std::rethrow_exception(caught), std::runtime_error);
}
