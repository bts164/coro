#include <filesystem>

#include <absl/log/globals.h>
#include <absl/log/log.h>
#include <absl/log/initialize.h>

#include <coro/coro.h>
#include <coro/io/file.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/sleep.h>

#include <xtensor/containers/xadapt.hpp>
#include <xtensor/containers/xtensor.hpp>
#include <xtensor/generators/xrandom.hpp>

#include "packet.h"

coro::Coro<int> async_main(int argc, char *argv[])
{
    using namespace std::chrono;
    using namespace std::chrono_literals;

    if (argc < 2) {
        LOG(ERROR) << "No path provided";
        co_return -1;
    }
    auto path = std::filesystem::absolute(argv[1]);
    LOG(INFO) << "Opening file " << path;
    auto file = co_await coro::File::open(
        path.string(),
        coro::FileMode::Write | coro::FileMode::Create | coro::FileMode::Truncate);

    Packet packet;
    std::vector<std::size_t> shape = {0x1<<20, 2};
    size_t N = sizeof(int16_t) * shape[0] * shape[1];
    std::vector<std::byte> buffer(sizeof(PacketHeader) + N + sizeof(PacketFooter));
    LOG(INFO) << "buffer size = " << buffer.size();
    auto t0 = system_clock::now();
    auto last_log = system_clock::now();
    for (size_t i = 0;; ++i) {
        auto t = system_clock::now();

        auto &header = *reinterpret_cast<PacketHeader*>(&buffer.at(0));
        auto data = xt::adapt(reinterpret_cast<int16_t*>(&buffer.at(sizeof(PacketHeader))), shape);
        auto &footer = *reinterpret_cast<PacketFooter*>(&buffer.at(sizeof(PacketHeader)+N));
        header.magic = PacketHeader::MAGIC;
        header.counter = i;
        header.packet_size_bytes = N;

        data = xt::random::rand<float>(shape, -100, 100);

        auto x = xt::linspace<float>(0, 3.5*M_PI, shape[0]);
        float phase = 2.0 * M_PI * duration_cast<milliseconds>(t-t0).count() / 2000.0;
        xt::view(data, xt::all(), 0) += 1000 * xt::sin(x+phase);
        xt::view(data, xt::all(), 1) += 1000 * xt::cos(x+phase);

        footer[0] = PacketFooter::MAGIC[0];
        footer[1] = PacketFooter::MAGIC[1];

        // Start the write (buffer moved in); sleep while it's in flight.
        std::size_t expected_n = buffer.size();
        auto wh = file.write(std::move(buffer), true);

        if (49 == (i%50)) {
            auto t = system_clock::now();
            LOG(INFO) << std::format("Wrote {} packets at {:.1f} pps to the stream",
                i, 50000.0 / duration_cast<milliseconds>(t - last_log).count());
            last_log = t;
        }
        auto next = t0 + i * 50ms;
        if (auto now = system_clock::now(); now < next) {
            co_await coro::sleep_for(next - now);
        }

        // Reclaim the buffer from the write result for reuse next iteration.
        auto [n, returned_buf] = co_await std::move(wh);
        if (n != expected_n) {
            LOG(FATAL) <<std::format("Only wrote {}/{} bytes", n, expected_n);
            co_return 1;
        }
        buffer = std::move(returned_buf);
    }
    co_return 0;
}

int main(int argc, char *argv[])
{
    absl::InitializeLog();
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

    coro::Runtime rt;
    return rt.block_on(async_main(argc, argv));
}