#include <sstream>
#include <random>

#include <absl/log/globals.h>
#include <absl/log/log.h>
#include <absl/log/initialize.h>

#include "plotsignal.pb.h"
#include "plotsignal.grpc.pb.h"
#include "grpc_server.h"

#include <coro/runtime/runtime.h>
#include <coro/runtime/single_threaded_executor.h>
#include <coro/sync/sleep.h>

using plotsignal::SignalService;
using plotsignal::SubscribeRequest;
using plotsignal::SubscribeResponse;
using plotsignal::SignalInfo;
using plotsignal::DataFrame;

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
        struct Defer { ~Defer() { LOG(INFO) << "Client disconnected"; } } defer;
        using namespace std::chrono;
        using namespace std::chrono_literals;
        LOG(INFO) << "New connection with target FPS: " << request.target_fps();
        SignalInfo info;
        info.set_x_start(0);
        info.set_x_end(4*M_PI);
        info.set_num_samples(2000);
        SubscribeResponse info_response;
        info_response.mutable_info()->CopyFrom(info);
        co_yield info_response;
        auto start_time = system_clock::now();
        auto last_log_time = start_time;
        std::size_t frame_count = 0;
        std::vector<float> y(2000);
        while (true) {
            auto frame_time = system_clock::now();
            double phase = duration_cast<milliseconds>(frame_time - start_time).count() / 1000.0;
            for (size_t i = 0; i < y.size(); ++i) {
                float x = (info.x_end() - info.x_start()) * i / (2000 - 1) + info.x_start();
                y[i] = std::sin(x + phase);
            }
            DataFrame frame;
            frame.mutable_y()->Assign(y.begin(), y.end());
            frame.set_timestamp(duration_cast<seconds>(frame_time - start_time).count());
            
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
            co_yield frame_response;
            frame_time += 1000ms / (long int)request.target_fps();
            if (auto now = system_clock::now(); now < frame_time) {
                co_await coro::sleep_for(frame_time - now);
            }
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