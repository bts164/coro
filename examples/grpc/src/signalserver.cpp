#include <sstream>
#include <random>
#include <fcntl.h>

#include <absl/log/globals.h>
#include <absl/log/log.h>
#include <absl/log/initialize.h>

#include "plotsignal.pb.h"
#include "plotsignal.grpc.pb.h"
#include "grpc_server.h"

#include <coro/runtime/runtime.h>
#include <coro/runtime/single_threaded_executor.h>
#include <coro/io/poll_stream.hpp>
#include <coro/sync/sleep.h>
#include <coro/task/spawn_blocking.h>

#include <xtensor/containers/xtensor.hpp>
#include <xtensor/containers/xadapt.hpp>
#include <xtensor/views/xview.hpp>

#include "packet.h"

using plotsignal::SignalService;
using plotsignal::SubscribeRequest;
using plotsignal::SubscribeResponse;
using plotsignal::SignalInfo;
using plotsignal::DataFrame;

struct HexView
{
    std::span<const std::byte> span;
};
template <>
struct std::formatter<HexView> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(HexView const &buffer, std::format_context& ctx) const {
        auto it = std::format_to(ctx.out(), "[");
        for (bool comma=false; auto byte : buffer.span) {
            if (std::exchange(comma, true)) {
                it = std::format_to(it, " {:#2x}", (unsigned char)byte);
            } else {
                it = std::format_to(it, "{:#2x}", (unsigned char)byte);
            }
        }
        return std::format_to(it, "]");
    }
};

class PacketDecoder {
public:
    using OutputType = Packet;

    PacketDecoder() = default;

    /// Decode one packet from byte buffer.
    ///
    /// Accumulates bytes incrementally so consumed > 0 whenever any input bytes
    /// are available. This is required because CircularByteBuffer::readable_span()
    /// returns only the contiguous region up to the wrap point — returning
    /// consumed=0 from a non-empty buffer would permanently stall PollStream's
    /// decode loop when a large payload crosses the buffer's wrap boundary.
    std::optional<Packet> decode(std::span<const std::byte> buffer, std::size_t& consumed)
    {
        consumed = 0;
        while (!buffer.empty()) {
            switch (m_state) {
            case State::ReadingHeader: {
                std::size_t take = std::min(sizeof(PacketHeader) - m_header_bytes, buffer.size());
                std::memcpy(reinterpret_cast<std::byte*>(&m_packet.header) + m_header_bytes,
                            buffer.data(), take);
                m_header_bytes += take;
                buffer = buffer.subspan(take);
                consumed += take;

                if (m_header_bytes < sizeof(PacketHeader)) return std::nullopt;
                m_header_bytes = 0;

                if (m_packet.header.magic != PacketHeader::MAGIC) {
                    throw std::runtime_error(std::format("Invalid packet header {}", m_packet.header));
                }
                m_payload_buf.clear();
                m_payload_buf.reserve(m_packet.header.packet_size_bytes);
                m_state = State::ReadingPayload;
                continue;
            }
            case State::ReadingPayload: {
                std::size_t take = std::min(
                    m_packet.header.packet_size_bytes - m_payload_buf.size(), buffer.size());
                m_payload_buf.insert(m_payload_buf.end(), buffer.data(), buffer.data() + take);
                buffer = buffer.subspan(take);
                consumed += take;

                if (m_payload_buf.size() < m_packet.header.packet_size_bytes) return std::nullopt;

                std::vector<std::size_t> shape = {m_packet.header.packet_size_bytes / 4, 2};
                // xt::xtensor assignment copies the data out of m_payload_buf
                m_packet.data = xt::adapt(reinterpret_cast<int16_t const*>(m_payload_buf.data()), shape);
                m_state = State::ReadingFooter;
                continue;
            }
            case State::ReadingFooter: {
                std::size_t take = std::min(sizeof(PacketFooter) - m_footer_bytes, buffer.size());
                std::memcpy(reinterpret_cast<std::byte*>(m_packet.footer.data()) + m_footer_bytes,
                            buffer.data(), take);
                m_footer_bytes += take;
                buffer = buffer.subspan(take);
                consumed += take;

                if (m_footer_bytes < sizeof(PacketFooter)) return std::nullopt;
                m_footer_bytes = 0;

                if (m_packet.footer[0] != PacketFooter::MAGIC[0] || m_packet.footer[1] != PacketFooter::MAGIC[1]) {
                    throw std::runtime_error(std::format("Invalid packet footer {}", m_packet.footer));
                }
                m_state = State::ReadingHeader;
                return std::exchange(m_packet, Packet());
            }}
        }
        return std::nullopt;
    }
    void reset();

private:
    enum class State {
        ReadingHeader,
        ReadingPayload,
        ReadingFooter,
    };

    State                  m_state        = State::ReadingHeader;
    Packet                 m_packet;
    std::size_t            m_header_bytes = 0;
    std::vector<std::byte> m_payload_buf;
    std::size_t            m_footer_bytes = 0;
};

template<std::invocable<> Fn>
class Defer
{
public:
    Defer(Fn &&fn) :
        m_active(true),
        m_fn(std::forward<Fn>(fn))
    {}
    Defer(Defer const &) = delete;
    Defer& operator=(Defer const &) = delete;
    ~Defer()
    {
        if (m_active) {
            m_fn();
        }
    }
private:
    bool m_active = false;
    Fn m_fn;
};
template<std::invocable<> Fn>
Defer<std::decay_t<Fn>> defer(Fn &&fn) {
    return Defer<std::decay_t<Fn>>(std::forward<Fn>(fn));
}

/*!
 * \brief Example gRPC server using coro::Runtime and the GrpcServer base class. 
 * The server listens for incoming SayHello RPCs and responds with a greeting message.
 */ 
class SignalServer : public GrpcServer<SignalServer, plotsignal::SignalService::AsyncService>
{
public:
    SignalServer() : GrpcServer(
        &plotsignal::SignalService::AsyncService::RequestSubscribe)
    {}

    coro::CoroStream<SubscribeResponse> Generate(SubscribeRequest const& request) {
        using namespace std::chrono;
        using namespace std::chrono_literals;
        try {
            auto logClosed = defer([]() {  LOG(INFO) << "Client disconnected"; });
            
            LOG(INFO) << "New connection with pipePath: " << request.pipepath();
            SignalInfo info;
            info.set_x_start(0);
            info.set_x_end(4*M_PI);
            SubscribeResponse info_response;
            info_response.mutable_info()->CopyFrom(info);
            co_yield info_response;

            auto start_time = system_clock::now();
            auto last_log_time = start_time;
            auto last_sleep_time = start_time;
            std::size_t frame_count = 0;

            auto path = std::filesystem::absolute(request.pipepath());
            int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
            if (fd < 0) {
                throw std::runtime_error(std::format("Failed opening pipe {}: {}", request.pipepath(), strerror(errno)));
            }
            auto closeFile = defer([=]() { close(fd); });

            auto stream = coro::PollStream<Packet, PacketDecoder>::open(
                fd, PacketDecoder(), coro::PollStreamOptions{ .backpressure = coro::BackpressureMode::Overrun });
            
            LOG(INFO) << "Starting stream";
            while (true) {
                DataFrame frame;
                for (size_t n = 0; n < 4; ++n) {
                    std::optional<Packet> packet;
                    try {
                        packet = co_await coro::next(stream);
                    } catch (coro::PollStreamOverrunError const& e) {
                        LOG(WARNING) << "Overrun: dropped " << e.missed() << " packets";
                        continue;
                    }
                    if (!packet) break;

                    auto frame_time = system_clock::now();
                    if (packet->data.shape(0) <= (0x1 <<12)) {
                        std::size_t b = frame.mutable_y()->size();
                        std::size_t e = b + sizeof(int16_t) * packet->data.size();
                        frame.mutable_y()->resize(e);
                        auto dst = xt::adapt(
                            reinterpret_cast<int16_t*>(frame.mutable_y()->data()+b),
                            packet->data.shape());
                        dst = packet->data;
                    } else {
                        using namespace xt::placeholders;
                        std::size_t step = std::ceil(std::log2(static_cast<float>(packet->data.shape(0))) - 12.0);
                        step = 0x1 << step;
                        auto src = xt::view(packet->data, xt::range(0, _, step), xt::all());

                        std::size_t b = frame.mutable_y()->size();
                        std::size_t e = b + sizeof(int16_t) * src.size();
                        
                        frame.mutable_y()->resize(e);
                        auto dst = xt::adapt(reinterpret_cast<int16_t*>(frame.mutable_y()->data() + b), src.shape());
                        dst = src;
                    }
                    frame.set_timestamp(duration_cast<seconds>(frame_time - start_time).count());
                }

                SubscribeResponse frame_response;
                frame_response.mutable_frame()->CopyFrom(frame);
                if (auto now = system_clock::now(); last_log_time + 2s < now) {
                    auto ms = duration_cast<milliseconds>(now - last_log_time);
                    LOG(INFO) << "Sent " << frame_count << " frames in the last "
                        << ms << ": " << (frame_count * 1000.0 / ms.count()) << " FPS";
                    last_log_time = now;
                    frame_count = 0;
                } else {
                    ++frame_count;
                }
                if (auto now = system_clock::now(); last_sleep_time + 8s < now) {
                    co_await coro::sleep_for(8s);
                    LOG(INFO) << "Woke from sleep";
                    last_sleep_time = system_clock::now();
                    co_yield frame_response;
                    LOG(INFO) << "Sent frame";
                } else {
                    co_yield frame_response;
                }
            }
            LOG(INFO) << "Exiting";
        } catch (std::exception const &e) {
            LOG(ERROR) << e.what();
        } catch (...) {
            LOG(ERROR) << "Unknown exception";
        }
    }

private:
};

int main(int argc, char *argv[])
{
    absl::InitializeLog();
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);
    uint16_t port = 50051;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    LOG(INFO) << "Starting SignalServer gRPC server on port " << port;
    // Register the SayHello RPC method with the base class, so it can spawn handlers for
    // incoming requests. This should be called in the constructor of the concrete server
    // for each RPC method it wants to handle.
    auto server = std::make_shared<SignalServer>();

    coro::Runtime rt(std::in_place_type<coro::SingleThreadedExecutor>);
    rt.block_on(SignalServer::Serve(server, port));
    return 0;
}