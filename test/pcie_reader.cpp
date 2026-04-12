/**
 * @file pcie_reader.cpp
 * @brief Example: Reading packets from a PCIe character device data stream
 *
 * This example demonstrates how to use PollStream with a custom decoder
 * (PcieDecoder) to read variable-length, multi-header packets from a
 * PCIe character device.
 *
 * Packet format:
 * - Header1 (16 bytes) - flags, payload length, timestamp
 * - Header2 (16 bytes, optional) - sequence number, reserved
 * - Payload (4KB-128KB variable) - actual data
 * - Footer (16 bytes) - magic pattern for sync validation
 *
 * Build:
 *   # This example is built as part of the examples target
 *
 * Usage:
 *   ./pcie_reader /dev/pcie_data
 */

#include "pcie_stream.h"
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

using namespace coro;

/**
 * @brief Process a single PCIe packet
 */
void process_packet(const PciePacket& pkt, std::size_t packet_num) {
    std::cout << "\n=== Packet #" << packet_num << " ===\n";
    std::cout << "Header1:\n";
    std::cout << "  Magic: 0x" << std::hex << pkt.header1.magic << std::dec << "\n";
    std::cout << "  Flags: 0x" << std::hex << pkt.header1.flags << std::dec << "\n";
    std::cout << "  Payload length: " << pkt.header1.payload_length << " bytes\n";
    std::cout << "  Timestamp: " << pkt.header1.timestamp << "\n";

    if (pkt.header2) {
        std::cout << "Header2:\n";
        std::cout << "  Sequence: " << pkt.header2->sequence_number << "\n";
    }

    std::cout << "Payload: " << pkt.payload.size() << " bytes\n";

    // Show first 16 bytes of payload
    std::cout << "Payload preview: ";
    for (std::size_t i = 0; i < std::min(pkt.payload.size(), std::size_t(16)); ++i) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(pkt.payload[i]) << " ";
    }
    std::cout << std::dec << "\n";

    std::cout << "Footer:\n";
    std::cout << "  Magic1: 0x" << std::hex << pkt.footer.magic1 << "\n";
    std::cout << "  Magic2: 0x" << pkt.footer.magic2 << std::dec << "\n";
}

/**
 * @brief Main coroutine: open PCIe character device and process packets
 */
Coro<int> pcie_reader(std::string device_path) {
    std::cout << "Opening PCIe character device: " << device_path << "\n";

    // Open device in non-blocking mode
    int fd = ::open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Error: Failed to open " << device_path << ": "
                  << strerror(errno) << "\n";
        co_return 1;
    }

    try {
        // Create PcieStream with decoder
        // Default buffer sizes: 256KB byte buffer, 64 packet buffer
        auto stream = PcieStream::open(fd, PcieDecoder{});

        std::cout << "Reading packets (Ctrl-C to stop)...\n";

        std::size_t packet_count = 0;
        std::size_t total_bytes = 0;

        // Read packets until EOF or error
        while (auto pkt = co_await next(stream)) {
            ++packet_count;
            total_bytes += pkt->payload.size();

            process_packet(*pkt, packet_count);

            // Optional: stop after N packets for testing
            // if (packet_count >= 10) break;
        }

        std::cout << "\n=== Summary ===\n";
        std::cout << "Total packets: " << packet_count << "\n";
        std::cout << "Total payload bytes: " << total_bytes << "\n";
        std::cout << "Stream closed (EOF)\n";

        co_return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error reading stream: " << e.what() << "\n";
        ::close(fd);
        co_return 1;
    }

    ::close(fd);
    co_return 0;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <pcie_device>\n";
        std::cerr << "Example: " << argv[0] << " /dev/pcie_data\n";
        return 1;
    }

    std::string device_path = argv[1];

    // Create runtime with single-threaded executor for simple example
    // For production, use multi-threaded: Runtime runtime(4);
    Runtime runtime(1);

    int result = runtime.block_on(pcie_reader(device_path));

    return result;
}
