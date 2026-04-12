/**
 * @file length_prefixed_stream.cpp
 * @brief Example: Reading length-prefixed messages from a pipe/socket
 *
 * This example demonstrates how to write a simple custom decoder for
 * PollStream. The protocol is a 4-byte network-order length prefix
 * followed by the message data.
 *
 * Protocol:
 * [4 bytes: length (network byte order)] [N bytes: data]
 *
 * This is a common pattern used by Protocol Buffers, JSON-RPC, and
 * many other protocols.
 */

#include <coro/io/poll_stream.h>
#include <coro/io/decoder_concept.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <arpa/inet.h>
#include <iomanip>
#include <iostream>
#include <vector>
#include <cstring>

using namespace coro;

/**
 * @brief Simple length-prefixed message decoder
 *
 * Decodes messages with 4-byte length prefix (network byte order).
 */
class LengthPrefixedDecoder {
public:
    using OutputType = std::vector<std::byte>;

    std::optional<std::vector<std::byte>> decode(std::span<const std::byte> buffer,
                                                  std::size_t& consumed) {
        consumed = 0;

        if (m_state == State::ReadingLength) {
            if (buffer.size() < 4) {
                return std::nullopt; // Need more bytes
            }

            // Read 4-byte length prefix (network byte order)
            uint32_t length_net;
            std::memcpy(&length_net, buffer.data(), 4);
            m_length = ntohl(length_net);

            // Validate length
            if (m_length > MAX_MESSAGE_SIZE) {
                throw std::runtime_error("Message too large: " + std::to_string(m_length));
            }

            buffer = buffer.subspan(4);
            consumed = 4;
            m_state = State::ReadingData;
        }

        // State::ReadingData
        if (buffer.size() < m_length) {
            return std::nullopt; // Need more bytes
        }

        // Read message data
        std::vector<std::byte> data(m_length);
        std::memcpy(data.data(), buffer.data(), m_length);
        consumed += m_length;

        // Reset for next message
        m_state = State::ReadingLength;
        m_length = 0;

        return data;
    }

    void reset() {
        m_state = State::ReadingLength;
        m_length = 0;
    }

private:
    enum class State { ReadingLength, ReadingData };
    State m_state = State::ReadingLength;
    uint32_t m_length = 0;
    static constexpr uint32_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024; // 16MB
};

// Verify decoder satisfies concept
static_assert(Decoder<LengthPrefixedDecoder>);

// Type alias for convenience
using LengthPrefixedStream = PollStream<std::vector<std::byte>, LengthPrefixedDecoder>;

/**
 * @brief Example: read messages from stdin
 */
Coro<int> read_messages() {
    std::cout << "Reading length-prefixed messages from stdin...\n";
    std::cout << "(Send 4-byte length + data)\n\n";

    // Create stream reading from stdin (fd 0)
    // Note: stdin must be made non-blocking for this to work
    auto stream = LengthPrefixedStream::open(0, LengthPrefixedDecoder{});

    std::size_t msg_count = 0;

    try {
        while (auto msg = co_await next(stream)) {
            ++msg_count;

            std::cout << "Message #" << msg_count << " (" << msg->size() << " bytes): ";

            // Print as string if printable, otherwise hex
            bool is_text = true;
            for (auto b : *msg) {
                if (!std::isprint(static_cast<unsigned char>(b))) {
                    is_text = false;
                    break;
                }
            }

            if (is_text) {
                for (auto b : *msg) {
                    std::cout << static_cast<char>(b);
                }
            } else {
                for (auto b : *msg) {
                    std::cout << std::hex << std::setw(2) << std::setfill('0')
                              << static_cast<int>(b) << " ";
                }
                std::cout << std::dec;
            }
            std::cout << "\n";
        }

        std::cout << "\nEOF - Total messages: " << msg_count << "\n";
        co_return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        co_return 1;
    }
}

int main() {
    std::cout << "Length-Prefixed Stream Example\n";
    std::cout << "================================\n\n";

    Runtime runtime(1);
    return runtime.block_on(read_messages());
}
