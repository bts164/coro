#include <sstream>
#include <random>

#include <absl/log/globals.h>
#include <absl/log/log.h>
#include <absl/log/initialize.h>

#include "helloworld.pb.h"
#include "helloworld.grpc.pb.h"
#include "grpc_server.h"

#include <coro/runtime/runtime.h>
#include <coro/runtime/single_threaded_executor.h>
#include <coro/sync/sleep.h>

using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

/*!
 * \brief Example gRPC server using coro::Runtime and the GrpcServer base class. 
 * The server listens for incoming SayHello RPCs and responds with a greeting message.
 */ 
class HelloWorldServer : public GrpcServer<HelloWorldServer, helloworld::Greeter::AsyncService>
{
    // Choose a random delay between 250ms and 1000ms to simulate work and demonstrate
    // concurrency with multiple requests. In a real server you would of course replace
    // this with actual processing logic.
    static std::chrono::milliseconds random_delay()
    {
        static std::random_device r;
        static std::default_random_engine e1(r());
        static std::uniform_int_distribution<int> uniform_dist(250, 1000);
        return std::chrono::milliseconds(uniform_dist(e1));
    }

public:
    HelloWorldServer() : GrpcServer(
        &helloworld::Greeter::AsyncService::RequestSayHello,
        &helloworld::Greeter::AsyncService::RequestSayHelloStreamRequest,
        &helloworld::Greeter::AsyncService::RequestSayHelloStreamReply,
        &helloworld::Greeter::AsyncService::RequestSayHelloBidiStream)
    {}

    /*!
    * \brief Example handler for the SayHello RPC.
    * The method signature is automatically determined by the ServiceMethodTraits specialization
    * for this RPC type, which extracts the request and reply types from the method pointer. The
    * base class will call this method for each incoming SayHello request, passing the deserialized
    * request message. The handler can co_await as needed to process the request and produce a reply,
    * which it returns to be sent back to the client.
    * */
    coro::Coro<HelloReply> Handle(HelloRequest const &request) {
        std::size_t i = m_request_count++;
        LOG(INFO) << "m_request_count: " << i;
        LOG(INFO) << "SayHello: name=\"" << request.name() << "\"";
        HelloReply reply;
        reply.set_message("Hello " + request.name() + " from request number " + std::to_string(i) + " to the coro gRPC server!");
        co_await coro::sleep_for(random_delay());
        LOG(INFO) << "SayHello: reply=\"" << reply.message() << "\"";
        co_return reply;
    }

    /*!
     * \brief Example handler for streaming SayHello RPC.
     * The method signature is automatically determined by the ServiceMethodTraits specialization
     * for this RPC type, which extracts the request and reply types from the method pointer. The
     * base class will call this method for each incoming streaming SayHello request, passing the deserialized
     * request message. The handler can co_await as needed to process the request and produce a reply,
     * which it returns to be sent back to the client.
     * */
   coro::Coro<HelloReply> Handle(coro::mpsc::Receiver<HelloRequest> req_rx) {
        std::size_t i = m_request_count++;
        LOG(INFO) << "m_request_count: " << i;
        LOG(INFO) << "SayHelloStreamRequest: receiving client stream";
        HelloReply reply;
        std::optional<std::string> previous_name;
        std::string names;
        int count = 0;
        while (auto req = co_await coro::next(req_rx)) {
            LOG(INFO) << "SayHelloStreamRequest: received name=\"" << (*req).name() << "\"";
            if (previous_name) {
                if (!names.empty()) names += ", ";
                names += *previous_name;
            }
            previous_name = (*req).name();
            ++count;
        }
        if (!previous_name.has_value()) {
            names = "nobody";
        } else {
            if (!names.empty()) names += ", & ";
            names += *previous_name;
        }
        reply.set_message("Hello " + names + " from request number " + std::to_string(i) + " to the coro gRPC server!");
        co_await coro::sleep_for(random_delay());
        LOG(INFO) << "SayHelloStreamRequest: " << count << " message(s) received, reply=\"" << reply.message() << "\"";
        co_return reply;
    }

    /*!
     * \brief Handler for the SayHelloStreamReply server-streaming RPC.
     *
     * Named Generate rather than Handle because C++ cannot overload on return
     * type — CoroStream<HelloReply> vs Coro<HelloReply> would be ambiguous.
     * Streams back one reply per word in the request name.
     */
    coro::CoroStream<HelloReply> Generate(HelloRequest const& request) {
        std::size_t i = m_request_count++;
        LOG(INFO) << "m_request_count: " << i;
        std::istringstream iss(request.name());
        std::vector<std::string> words;
        for (std::string word; iss >> word;)
            words.push_back(word);

        LOG(INFO) << "SayHelloStreamReply: name=\"" << request.name()
                  << "\", streaming " << words.size() << " reply(s)";
        for (size_t j = 0; j < words.size(); ++j) {
            HelloReply reply;
            reply.set_message("Hello " + words[j] + "! (" +
                std::to_string(j + 1) + " of " + std::to_string(words.size()) + " in request number " + std::to_string(i) + ")");
            co_await coro::sleep_for(random_delay());
            LOG(INFO) << "SayHelloStreamReply: sending reply " << (j + 1)
                      << "/" << words.size() << ": \"" << reply.message() << "\"";
            co_yield reply;
        }
        LOG(INFO) << "SayHelloStreamReply: stream complete";
    }

    /*!
     * \brief Handler for the SayHelloBidiStream bidi-streaming RPC.
     *
     * Named Process rather than Handle or Generate to avoid the overload-by-
     * return-type problem (CoroStream vs Coro). Echoes each request back as a
     * greeting, one reply per request, until the client half-closes.
     */
    coro::CoroStream<HelloReply> Process(coro::mpsc::Receiver<HelloRequest> req_rx) {
        std::size_t i = m_request_count++;
        LOG(INFO) << "m_request_count: " << i;
        LOG(INFO) << "SayHelloBidiStream: session started";
        int count = 0;
        while (auto req = co_await coro::next(req_rx)) {
            LOG(INFO) << "SayHelloBidiStream: received \"" << (*req).name() << "\"";
            HelloReply reply;
            reply.set_message("Echo from request number " + std::to_string(i) + ": " + (*req).name());
            co_await coro::sleep_for(random_delay());
            co_yield reply;
            ++count;
        }
        LOG(INFO) << "SayHelloBidiStream: session complete, " << count << " exchange(s)";
    }

private:

    // Example of server state that can be accessed and modified by handlers.
     // You should be careful about this in general, but in this case because we're
     // explicitly using a single-threaded executor we don't need any mutexes or other
     // synchronization primitives to protect it
     std::size_t m_request_count = 0;
};

int main(int argc, char *argv[])
{
    absl::InitializeLog();
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);
    uint16_t port = 50051;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    LOG(INFO) << "Starting HelloWorld gRPC server on port " << port;
    // Register the SayHello RPC method with the base class, so it can spawn handlers for
    // incoming requests. This should be called in the constructor of the concrete server
    // for each RPC method it wants to handle.
    auto server = std::make_shared<HelloWorldServer>();

    coro::Runtime rt(std::in_place_type<coro::SingleThreadedExecutor>);
    rt.block_on(HelloWorldServer::Serve(server, port));
    return 0;
}