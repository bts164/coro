#include "helloworld.pb.h"
#include "helloworld.grpc.pb.h"
#include "grpc_server.h"

using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

/*!
 * \brief Example gRPC server using coro::Runtime and the GrpcServer base class. 
 * The server listens for incoming SayHello RPCs and responds with a greeting message.
 */ 
class HelloWorldServer : public GrpcServer<HelloWorldServer, helloworld::Greeter::AsyncService>
{
    // Allow the base class to call the private Handle() method, which is the RPC handler for this server.
    friend class GrpcServer<HelloWorldServer, helloworld::Greeter::AsyncService>;

public:
    HelloWorldServer(uint16_t port) :
        GrpcServer(port)
    {}

private:
    /*!
    * \brief Example handler for the SayHello RPC.
    * The method signature is automatically determined by the ServiceMethodTraits specialization
    * for this RPC type, which extracts the request and reply types from the method pointer. The
    * base class will call this method for each incoming SayHello request, passing the deserialized
    * request message. The handler can co_await as needed to process the request and produce a reply,
    * which it returns to be sent back to the client.
    * */
    coro::Coro<HelloReply> Handle(HelloRequest const &request) {
        HelloReply reply;
        reply.set_message("Hello " + request.name() + " from the coro gRPC server!");
        co_return reply;
    }
};

int main(int argc, char *argv[])
{
    uint16_t port = 50051;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    auto server = std::make_shared<HelloWorldServer>(port);
    // Register the SayHello RPC method with the base class, so it can spawn handlers for
    // incoming requests. This should be called in the constructor of the concrete server
    // for each RPC method it wants to handle.
    server->RegisterRequestType(&helloworld::Greeter::AsyncService::RequestSayHello);

    coro::Runtime rt;
    rt.block_on(HelloWorldServer::Serve(server));
    return 0;
}