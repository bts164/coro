#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/sync/oneshot.h>
#include <coro/runtime/runtime.h>
#include <coro/task/join_set.h>

// using grpc::Server;
// using grpc::ServerAsyncResponseWriter;
// using grpc::ServerBuilder;
// using grpc::ServerCompletionQueue;
// using grpc::ServerContext;
// using grpc::Status;

/*!
 * \brief Traits for extracting method information from gRPC service methods.
 */
template<typename Method>
struct ServiceMethodTraits;

/*!
* \brief Specialization of ServiceMethodTraits extracting method information from unary RPC service methods.
*/
template<typename Service, typename Request, typename Reply>
struct ServiceMethodTraits<void (Service::*)(grpc::ServerContext*, Request*, grpc::ServerAsyncResponseWriter<Reply>*, grpc::CompletionQueue*, grpc::ServerCompletionQueue*, void*)> {
    using ServiceType = Service;
    using MethodType = void (Service::*)(grpc::ServerContext*, Request*, grpc::ServerAsyncResponseWriter<Reply>*, grpc::CompletionQueue*, grpc::ServerCompletionQueue*, void*);
    using RequestType = Request;
    using ReplyType = Reply;
};

/*!
* \brief Specialization of ServiceMethodTraits extracting method information from client-streaming RPC service methods.
*/
template<typename Service, typename Request, typename Reply>
struct ServiceMethodTraits<void (Service::*)(grpc::ServerContext*, grpc::ServerAsyncReader<Request, Request>*, grpc::ServerAsyncResponseWriter<Reply>*, grpc::ServerCompletionQueue*, grpc::ServerCompletionQueue*, void*)> {
    using ServiceType = Service;
    using MethodType = void (Service::*)(grpc::ServerContext*, grpc::ServerAsyncReader<Request, Request>*, grpc::ServerAsyncResponseWriter<Reply>*, grpc::ServerCompletionQueue*, grpc::ServerCompletionQueue*, void*);
    using RequestType = Request;
    using ReplyType = Reply;
};

/*!
* \brief Specialization of ServiceMethodTraits extracting method information from server-streaming RPC service methods.
*/
template<typename Service, typename Request, typename Reply>
struct ServiceMethodTraits<void (Service::*)(grpc::ServerContext*, grpc::ServerAsyncReader<Request, Reply>*, grpc::ServerAsyncWriter<Reply>*, grpc::ServerCompletionQueue*, grpc::ServerCompletionQueue*, void*)> {
    using ServiceType = Service;
    using MethodType = void (Service::*)(grpc::ServerContext*, Request *, grpc::ServerAsyncWriter<Reply>*, grpc::ServerCompletionQueue*, grpc::ServerCompletionQueue*, void*);
    using RequestType = Request;
    using ReplyType = Reply;
};

/*!
* \brief Specialization of ServiceMethodTraits extracting method information from server-streaming RPC service methods.
*/
template<typename Service, typename Request, typename Reply>
struct ServiceMethodTraits<void (Service::*)(grpc::ServerContext*, grpc::ServerAsyncReaderWriter<Reply, Request>*, grpc::ServerCompletionQueue*, grpc::ServerCompletionQueue*, void*)> {
    using ServiceType = Service;
    using MethodType = void (Service::*)(grpc::ServerContext*, Request *, grpc::ServerAsyncWriter<Reply>*, grpc::ServerCompletionQueue*, grpc::ServerCompletionQueue*, void*);
    using RequestType = Request;
    using ReplyType = Reply;
};

/*!
 * \brief Base class for implementing gRPC servers using coro::Runtime.
 */
template<typename ConcreteServer ,typename Service> // e.g. Service = helloworld
class GrpcServer : public std::enable_shared_from_this<GrpcServer<ConcreteServer, Service>>
{
private:
    /*!
     * \brief Handles the next incoming request for a specific RPC method.
     * This method is called by the base class for each incoming request of the given method type,
     * and is responsible for spawning a new handler for the next request of that type, then processing
     * the current request to completion. The method signature is determined by the ServiceMethodTraits
     * specialization for the given method pointer,
     * \arg self A shared pointer to the concrete server instance, used for spawning the next handler.
     * \arg method A pointer to the Request method on the service's AsyncService class for the RPC type to
     * handle, e.g. &helloworld::Greeter::AsyncService::RequestSayHello.
     * \returns a Coro that completes when the request has been fully handled and the response has been sent.
     */
    template<typename Method>
    static coro::Coro<void> HandleNextRequest(std::shared_ptr<ConcreteServer> self, Method method)
    {
        using RequestType = typename ServiceMethodTraits<Method>::RequestType;
        using ReplyType = typename ServiceMethodTraits<Method>::ReplyType;
        auto [tx, rx] = coro::oneshot::channel<int>();
        grpc::ServerContext ctx;
        RequestType request;
        grpc::ServerAsyncResponseWriter<ReplyType> responder(&ctx);
        (self->m_service.*method)(&ctx, &request, &responder, self->m_cq.get(), self->m_cq.get(), &tx);
        co_await rx; // Wait for RPC to be received
        
        // Spawn a new task to handle the the next one, then handle this one to completion.
        self->m_join_set.spawn(HandleNextRequest(self, method));

        // create a new oneshot channel for the finish callback, and pass the sender as the tag. When the RPC is finished, the callback will complete the channel and wake this task to let it know it can safely destroy the responder.
        std::tie(tx, rx) = coro::oneshot::channel<int>();
        ReplyType reply = co_await self->Handle(request);
        responder.Finish(reply, grpc::Status::OK, &tx);
    
        // Wait for the finish callback before cleaning up the responder, since it may still be accessing it until then.
        co_await rx; 
    }

protected:
    explicit GrpcServer(uint16_t port)
        : m_port(port)
    {}

public:
    /*
    * \brief Registers an RPC method to be handled by this server. Must be called in the constructor
    * of the concrete server before entering the runtime, so that the initial request handlers can be
    * constructed and stored for spawning later in Serve(). The method should be a pointer to a Request
    * method on the service's AsyncService class, e.g. &helloworld::Greeter::AsyncService::RequestSayHello.
    */
    void RegisterRequestType(auto method) {
        // not in the async runtime yet, so we can't spawn the initial handlers, but
        // we can construct them and store them in a vector to be spawned later in Serve().
        m_initial_request_handlers.emplace_back(HandleNextRequest(
            std::static_pointer_cast<ConcreteServer>(this->shared_from_this()), method));
    }

public:
    /*!
     * \brief Starts the gRPC server and begins handling incoming requests
     * \arg self A shared pointer to the concrete server instance, used for spawning request handlers.
     */
    static coro::Coro<void> Serve(std::shared_ptr<ConcreteServer> self) {
        std::string address = absl::StrFormat("0.0.0.0:%d", self->m_port);
        grpc::ServerBuilder builder;
        builder.AddListeningPort(address, grpc::InsecureServerCredentials());
        builder.RegisterService(&self->m_service);
        self->m_cq = builder.AddCompletionQueue();
        std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
        
        // Now that we're in the runtime, spawn the initial request handlers for each RPC type
        // to get things going. Each handler will spawn a new one when it receives a request, so
        // there will always be one handler per RPC type waiting for the next request.
        for (auto& handler : self->m_initial_request_handlers) {
            self->m_join_set.spawn(std::move(handler));
        }

        // Start the GRPC server's internal completion queue loop on a blocking thread, so it doesn't
        // block the async runtime. The loop will run until the server is shut down and the completion
        // queue is drained, at which point all the request handlers will have been completed and the
        // join set will be empty, so we can just wait for that to happen with co_await.
        co_await coro::spawn_blocking([=] () {
            while (true) {
                bool ok = false;
                void* tag = nullptr;
                if (!self->m_cq->Next(&tag, &ok) || !ok) {
                    // CQ is shutting down or an error occurred, break the loop to allow the server to clean up and exit.
                    break;
                }
                auto tx = static_cast<coro::oneshot::OneshotSender<int>*>(tag);
                tx->send(0);
            }
        });
    }
    
private:
    //! Port to listen on, set in constructor
    uint16_t m_port;
    //! gRPC service instance, e.g. helloworld::Greeter::AsyncService
    Service m_service;
    //! JoinSet for all active request handlers, used to track when the server can shut down after shutdown is called and the completion queue is drained.
    coro::JoinSet<void> m_join_set;
    //! gRPC completion queue for handling async events from the server, initialized in Serve() after the server is started.
    std::unique_ptr<grpc::ServerCompletionQueue> m_cq;
    
    /*!
     * For each RPC type, we need to post an initial request handler that will
     * be waiting for the first request to arrive. When it does, it will spawn a
     * new handler for the next request of that same type, so there will always be
     * one handler per RPC type waiting for the next request. We store these initial
     * handlers in a vector because the coroutines are constructed before we enter
     * the runtime, so we can't spawn them yet. Once we enter the runtime in Serve(),
     * we can spawn them all to get things going.
     */
    std::vector<coro::Coro<void>> m_initial_request_handlers;
};
