/**
 * @file length_prefixed_stream.cpp
 * @brief Example: Reading length-prefixed messages from a pipe/socket using PollStream.
 *
 * Demonstrates how to write a custom decoder for PollStream using a DecoderStream
 * coroutine. The protocol is a 4-byte network-order length prefix followed by the
 * message payload — a common framing used by Protocol Buffers, JSON-RPC, and many
 * other protocols.
 *
 * Protocol:
 *   [4 bytes: length (network byte order)] [N bytes: data]
 *
 * The decoder is expressed as a simple sequential coroutine — no explicit state machine
 * is needed. Each `co_await src.memcpy(...)` suspends until the required bytes arrive;
 * the coroutine frame carries all partial-read state across suspension points.
 */

#include <coro/io/poll_stream.h>
#include <coro/io/decoder_stream.h>
#include <coro/io/byte_source.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <arpa/inet.h>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace coro;

/// Maximum accepted message payload size. Messages exceeding this limit cause the
/// stream to fault with a std::runtime_error.
static constexpr uint32_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024; // 16 MB

/**
 * @brief Decoder coroutine for the length-prefixed message protocol.
 *
 * Reads a 4-byte big-endian length prefix followed by that many bytes of payload,
 * yielding each complete message as a `std::vector<std::byte>`. Loops until the
 * source signals clean EOF between messages, or throws on a framing error.
 *
 * @param src ByteSource supplying raw bytes from the fd.
 * @throws std::runtime_error if a message length exceeds MAX_MESSAGE_SIZE.
 */
DecoderStream<std::vector<std::byte>> length_prefixed_decoder(ByteSource& src) {
    for (;;) {
        // Read 4-byte length prefix. Returns 0 on clean EOF between messages.
        uint32_t length_net = 0;
        if (co_await src.memcpy(&length_net, sizeof(length_net)) == 0) co_return;

        uint32_t length = ntohl(length_net);
        if (length > MAX_MESSAGE_SIZE)
            throw std::runtime_error("Message too large: " + std::to_string(length));

        std::vector<std::byte> data(length);
        co_await src.memcpy(data.data(), length);
        co_yield std::move(data);
    }
}

/**
 * @brief Read and display length-prefixed messages from stdin until EOF.
 *
 * Printable messages are displayed as text; binary messages are shown as hex.
 *
 * @return 0 on clean EOF, 1 on error.
 */
Coro<int> read_messages() {
    std::cout << "Reading length-prefixed messages from stdin...\n";
    std::cout << "(Send 4-byte length + data)\n\n";

    // stdin (fd 0) must be made non-blocking before use with PollStream.
    auto stream = PollStream<std::vector<std::byte>>::open(0, length_prefixed_decoder);

    std::size_t msg_count = 0;
    try {
        while (auto msg = co_await next(stream)) {
            ++msg_count;
            std::cout << "Message #" << msg_count << " (" << msg->size() << " bytes): ";

            bool is_text = true;
            for (auto b : *msg)
                if (!std::isprint(static_cast<unsigned char>(b))) { is_text = false; break; }

            if (is_text) {
                for (auto b : *msg) std::cout << static_cast<char>(b);
            } else {
                for (auto b : *msg)
                    std::cout << std::hex << std::setw(2) << std::setfill('0')
                              << static_cast<int>(b) << " ";
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
