#pragma once

#include <coro/io/poll_stream.h>
#include "pcie_decoder.h"
#include "pcie_packet.h"

namespace coro {

/**
 * @brief Type alias for PCIe character device packet stream.
 *
 * Convenience alias for PollStream specialized for the PCIe character device protocol.
 *
 * Usage:
 *   int fd = open("/dev/pcie_data", O_RDONLY | O_NONBLOCK);
 *   auto stream = PcieStream::open(fd, PcieDecoder{});
 *
 *   while (auto pkt = co_await stream.next()) {
 *       process_packet(*pkt);
 *   }
 */
using PcieStream = PollStream<PciePacket, PcieDecoder>;

} // namespace coro
