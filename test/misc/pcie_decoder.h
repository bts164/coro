#pragma once

#include <coro/io/decoder_stream.h>
#include <coro/io/byte_source.h>
#include "pcie_packet.h"

namespace coro {

/**
 * @brief Decoder coroutine for the PCIe character device packet protocol.
 *
 * Decodes packets with the structure:
 *   Header1 (16 bytes) — magic, flags, payload_length, timestamp
 *   Header2 (16 bytes, optional) — present if header1.has_header2
 *   Payload (variable, 4KB–128KB)
 *   Footer  (16 bytes) — magic pattern for frame sync validation
 *
 * Usage:
 *   auto stream = PollStream<PciePacket>::open(fd, pcie_decoder);
 *   while (auto pkt = co_await next(stream)) { ... }
 */
DecoderStream<PciePacket> pcie_decoder(ByteSource& src);

} // namespace coro
