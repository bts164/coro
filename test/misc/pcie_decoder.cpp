#include "pcie_decoder.h"
#include <stdexcept>

namespace coro {

DecoderStream<PciePacket> pcie_decoder(ByteSource& src) {
    for (;;) {
        PciePacket pkt{};

        if (co_await src.memcpy(&pkt.header1, sizeof(pkt.header1)) == 0)
            co_return; // clean EOF between packets

        if (pkt.header1.payload_length < PCIE_MIN_PAYLOAD_SIZE ||
            pkt.header1.payload_length > PCIE_MAX_PAYLOAD_SIZE)
            throw std::runtime_error(
                "pcie_decoder: invalid payload_length " +
                std::to_string(pkt.header1.payload_length));

        if (pkt.header1.has_header2) {
            pkt.header2.emplace();
            co_await src.memcpy(&*pkt.header2, sizeof(PciePacket::Header2));
        }

        pkt.payload.resize(pkt.header1.payload_length);
        co_await src.memcpy(pkt.payload.data(), pkt.header1.payload_length);

        co_await src.memcpy(&pkt.footer, sizeof(pkt.footer));

        if (pkt.footer.magic1 != PCIE_FOOTER_MAGIC1 ||
            pkt.footer.magic2 != PCIE_FOOTER_MAGIC2)
            throw std::runtime_error(
                "pcie_decoder: footer magic mismatch — frame sync lost");

        co_yield std::move(pkt);
    }
}

} // namespace coro
