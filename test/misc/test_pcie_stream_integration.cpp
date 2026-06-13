#include <gtest/gtest.h>
#include "pcie_stream.h"
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <unistd.h>
#include <fcntl.h>
#include <thread>
#include <vector>

using namespace coro;

// ---------------------------------------------------------------------------
// Helper: Simulated PCIe Character Device (pipe + data generator)
// ---------------------------------------------------------------------------

class SimulatedPcieDevice {
public:
    SimulatedPcieDevice() {
        int fds[2];
        if (pipe(fds) != 0) {
            throw std::runtime_error("pipe() failed");
        }
        m_read_fd = fds[0];
        m_write_fd = fds[1];

        // Set read end non-blocking
        int flags = fcntl(m_read_fd, F_GETFL);
        fcntl(m_read_fd, F_SETFL, flags | O_NONBLOCK);
    }

    ~SimulatedPcieDevice() {
        stop_generator();
        if (m_read_fd >= 0) ::close(m_read_fd);
        if (m_write_fd >= 0) ::close(m_write_fd);
    }

    int read_fd() const { return m_read_fd; }

    void write_packet(const PciePacket& pkt) {
        // TODO: Phase 3 - Serialize packet to wire format
        // - Write header1
        // - Write header2 if present
        // - Write payload
        // - Write footer
    }

    void start_generator(std::size_t num_packets, std::size_t payload_size) {
        // TODO: Phase 3 - Start background thread that generates packets
        // Simulates PCIe device streaming packets at regular intervals
        m_generator_thread = std::thread([this, num_packets, payload_size]() {
            for (std::size_t i = 0; i < num_packets; ++i) {
                PciePacket pkt;
                // TODO: Fill in packet fields
                write_packet(pkt);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            // Close write end to signal EOF
            ::close(m_write_fd);
            m_write_fd = -1;
        });
    }

    void stop_generator() {
        if (m_generator_thread.joinable()) {
            m_generator_thread.join();
        }
    }

private:
    int         m_read_fd = -1;
    int         m_write_fd = -1;
    std::thread m_generator_thread;
};

// ---------------------------------------------------------------------------
// Integration tests
// ---------------------------------------------------------------------------

TEST(PcieStreamIntegrationTest, ReadSinglePacket) {
    // TODO: Phase 3 - Implement test
    // - Create SimulatedPcieDevice
    // - Write one complete packet
    // - Open PcieStream
    // - co_await stream.next()
    // - Verify packet received correctly with all fields
}

TEST(PcieStreamIntegrationTest, ReadStreamOfPackets) {
    // TODO: Phase 3 - Implement test
    // - Start generator producing 100 packets
    // - Open PcieStream
    // - Read all packets in loop
    // - Verify count and footer magic on each
}

TEST(PcieStreamIntegrationTest, VariablePayloadSizes) {
    // TODO: Phase 3 - Implement test
    // - Write packets with varying payload sizes (4KB, 32KB, 128KB)
    // - Verify all decoded correctly
}

TEST(PcieStreamIntegrationTest, PacketsWithAndWithoutHeader2) {
    // TODO: Phase 3 - Implement test
    // - Write some packets with header2, some without
    // - Verify header2 presence detected correctly
}

TEST(PcieStreamIntegrationTest, InvalidFooterMagic_StreamFaults) {
    // TODO: Phase 3 - Implement test
    // - Write packet with corrupted footer
    // - Verify stream throws exception with framing error
}

TEST(PcieStreamIntegrationTest, LargePayload_ZeroCopy) {
    // TODO: Phase 3 - Implement test to verify zero-copy optimization
    // - Write packet with 128KB payload
    // - Verify no extra memcpy (would need instrumentation)
    // - At minimum verify packet received correctly
}

TEST(PcieStreamIntegrationTest, MultipleStreamsSimultaneous) {
    // TODO: Phase 3 - Implement test
    // - Create 4 SimulatedPcieDevice instances
    // - Open 4 PcieStreams
    // - Generate packets on all 4 simultaneously
    // - Verify each stream receives correct packets independently
}

TEST(PcieStreamIntegrationTest, Backpressure_SlowConsumer) {
    // TODO: Phase 3 - Implement test
    // - Start fast generator (packets arriving faster than consumed)
    // - Verify backpressure activates (polling stops)
    // - Consume packets slowly
    // - Verify no packets lost
}
