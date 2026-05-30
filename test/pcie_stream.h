#pragma once

#include <coro/io/poll_stream.h>
#include "pcie_decoder.h"
#include "pcie_packet.h"

namespace coro {

/**
 * @brief PollStream specialisation for the PCIe character device protocol.
 *
 * Usage:
 * @code
 * int fd = open("/dev/pcie_data", O_RDONLY | O_NONBLOCK);
 * auto stream = PcieStream::open(fd, pcie_decoder);
 * while (auto pkt = co_await next(stream)) {
 *     process(*pkt);
 * }
 * @endcode
 */
using PcieStream = PollStream<PciePacket>;

} // namespace coro
