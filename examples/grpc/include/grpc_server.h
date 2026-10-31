#pragma once

#include <chrono>
#include <memory>
#include <optional>

#include <absl/log/log.h>
#include <grpcpp/grpcpp.h>

#include <coro/co_invoke.h>
#include <coro/coro.h>
#include <coro/coro_stream.h>
#include <coro/io/signal.h>
#include <coro/sync/oneshot.h>
#include <coro/sync/mpsc.h>
#include <coro/sync/select.h>
#include <coro/sync/when.h>
#include <coro/runtime/runtime.h>
#include <coro/task/join_set.h>
#include <coro/task/spawn_blocking.h>

enum class RpcType {
    Unary,
    ClientStream,
    ServerStream,
    BidiStream
};

constexpr const char* RpcTypeName(RpcType type) {
    switch (type) {
        case RpcType::Unary:        return "unary";
        case RpcType::ClientStream: return "client-streaming";
        case RpcType::ServerStream: return "server-streaming";
        case RpcType::BidiStream:   return "bidi-streaming";
    }
    return "unknown";
}
using RpcTypeUnaryTag        = std::integral_constant<RpcType, RpcType::Unary>;
using RpcTypeClientStreamTag = std::integral_constant<RpcType, RpcType::ClientStream>;
using RpcTypeServerStreamTag = std::integral_constant<RpcType, RpcType::ServerStream>;
using RpcTypeBidiStreamTag   = std::integral_constant<RpcType, RpcType::BidiStream>;

/*!
 * \brief Traits for extracting method information from gRPC service methods.
 */
template<typename Method>
struct ServiceMethodTraits;

/*!
 * \brief Unary RPC: Request* → AsyncResponseWriter<Reply>*
 */
template<typename Service, typename Request, typename Reply>
struct ServiceMethodTraits<void (Service::*)(grpc::ServerContext*, Request*,
    grpc::ServerAsyncResponseWriter<Reply>*,
    grpc::CompletionQueue*, grpc::ServerCompletionQueue*, void*)>
{
    static constexpr RpcType RPC_TYPE = RpcType::Unary;
    using RpcTypeTag    = RpcTypeUnaryTag;
    using ServiceType   = Service;
    using RequestType   = Request;
    using ReplyType     = Reply;
};

/*!
 * \brief Client-streaming RPC: ServerAsyncReader<Reply, Request>*
 */
template<typename Service, typename Request, typename Reply>
struct ServiceMethodTraits<void (Service::*)(grpc::ServerContext*,
    grpc::ServerAsyncReader<Reply, Request>*,
    grpc::CompletionQueue*, grpc::ServerCompletionQueue*, void*)>
{
    static constexpr RpcType RPC_TYPE = RpcType::ClientStream;
    using RpcTypeTag    = RpcTypeClientStreamTag;
    using ServiceType   = Service;
    using RequestType   = Request;
    using ReplyType     = Reply;
};

/*!
 * \brief Server-streaming RPC: Request* → ServerAsyncWriter<Reply>*
 */
template<typename Service, typename Request, typename Reply>
struct ServiceMethodTraits<void (Service::*)(grpc::ServerContext*, Request*,
    grpc::ServerAsyncWriter<Reply>*,
    grpc::CompletionQueue*, grpc::ServerCompletionQueue*, void*)>
{
    static constexpr RpcType RPC_TYPE = RpcType::ServerStream;
    using RpcTypeTag    = RpcTypeServerStreamTag;
    using ServiceType   = Service;
    using RequestType   = Request;
    using ReplyType     = Reply;
};

/*!
 * \brief Bidi-streaming RPC: ServerAsyncReaderWriter<Reply, Request>*
 */
template<typename Service, typename Request, typename Reply>
struct ServiceMethodTraits<void (Service::*)(grpc::ServerContext*,
    grpc::ServerAsyncReaderWriter<Reply, Request>*,
    grpc::CompletionQueue*, grpc::ServerCompletionQueue*, void*)>
{
    static constexpr RpcType RPC_TYPE = RpcType::BidiStream;
    using RpcTypeTag    = RpcTypeBidiStreamTag;
    using ServiceType   = Service;
    using RequestType   = Request;
    using ReplyType     = Reply;
};

/*!
 * \brief Base class for implementing gRPC servers using coro::Runtime.
 */
template<typename ConcreteServer, typename Service>
class GrpcServer : public std::enable_shared_from_this<GrpcServer<ConcreteServer, Service>>
{
private:

    // -------------------------------------------------------------------------
    // Unary RPC handler
    // -------------------------------------------------------------------------

    template<typename Method>
    static coro::Coro<void> HandleNextRequest(
        RpcTypeUnaryTag,
        std::shared_ptr<ConcreteServer> self,
        Method method)
    {
        using RequestType = typename ServiceMethodTraits<Method>::RequestType;
        using ReplyType   = typename ServiceMethodTraits<Method>::ReplyType;

        grpc::ServerContext ctx;
        RequestType request;
        grpc::ServerAsyncResponseWriter<ReplyType> responder(&ctx);

        // Register for the next incoming unary call. Tag fires (ok=true) when
        // a client request has been received and is ready to process.
        {
            auto [start_tx, start_rx] = coro::oneshot_channel<bool>();
            (self->m_service.*method)(&ctx, &request, &responder,
                self->m_cq.get(), self->m_cq.get(), &start_tx);
            auto start_rx_result = co_await start_rx.recv();
                if (!start_rx_result.has_value() || !start_rx_result.value()) co_return; // Failed to start (server shutting down)
        }

        // Post a handler for the next incoming call before processing this one.
        self->m_join_set.spawn(RpcType::Unary,
            HandleNextRequest(RpcTypeUnaryTag{}, self, method));

        // Process the request and send the reply.
        ReplyType reply = co_await self->Handle(request);

        auto [finish_tx, finish_rx] = coro::oneshot_channel<bool>();
        responder.Finish(reply, grpc::Status::OK, &finish_tx);
        (void)co_await finish_rx.recv();
    }

    // -------------------------------------------------------------------------
    // Client-streaming RPC handler
    //
    // gRPC async client-streaming flow:
    //   1. RequestMethod() — register for the next call; tag fires when a client
    //      connects and the RPC is established.
    //   2. reader.Read() in a loop — each call posts one completion to the CQ.
    //      ok=true: a message was received into `request`.
    //      ok=false: client has half-closed (done sending). Normal end of stream.
    //   3. reader.Finish() — send the single reply; tag fires when delivered.
    //
    // The stream of incoming requests is forwarded to Handle() via an mpsc channel.
    // Dropping the sender closes the channel, which signals EOF to Handle().
    // -------------------------------------------------------------------------

    template<typename Method>
    static coro::Coro<void> HandleNextRequest(
        RpcTypeClientStreamTag,
        std::shared_ptr<ConcreteServer> self,
        Method method)
    {
        using RequestType = typename ServiceMethodTraits<Method>::RequestType;
        using ReplyType   = typename ServiceMethodTraits<Method>::ReplyType;

        grpc::ServerContext ctx;
        grpc::ServerAsyncReader<ReplyType, RequestType> reader(&ctx);

        // Register for the next incoming client-streaming call.
        {
            auto [start_tx, start_rx] = coro::oneshot_channel<bool>();
            (self->m_service.*method)(&ctx, &reader,
                self->m_cq.get(), self->m_cq.get(), &start_tx);
            auto start_rx_result = co_await start_rx.recv();
            if (!start_rx_result.has_value() || !start_rx_result.value()) co_return; // Failed to start (server shutting down)
        }

        // Post a handler for the next incoming call before processing this one.
        self->m_join_set.spawn(RpcType::ClientStream,
            HandleNextRequest(RpcTypeClientStreamTag{}, self, method));

        auto stream = [](auto self, auto &reader) ->coro::CoroStream<RequestType> {
            // Read messages from the client and forward them into the channel.
            for (size_t i = 0; true; ++i) {
                RequestType request;
                auto [read_tx, read_rx] = coro::oneshot_channel<bool>();
                reader.Read(&request, &read_tx);
                auto read_rx_result = co_await read_rx.recv();
                if (!read_rx_result.has_value() || !read_rx_result.value()) break; // ok=false: client done sending (half-close)
                co_yield std::move(request);
            }
            co_return;
        }(self, reader);

        // Wait for Handle() to produce the reply.
        ReplyType reply = co_await self->Handle(std::move(stream));

        // Send the single reply and wait for delivery.
        auto [finish_tx, finish_rx] = coro::oneshot_channel<bool>();
        reader.Finish(reply, grpc::Status::OK, &finish_tx);
        (void)co_await finish_rx.recv();
    }

    // -------------------------------------------------------------------------
    // Server-streaming RPC handler
    //
    // gRPC async server-streaming flow:
    //   1. RequestMethod() — register for the next call; tag fires when a client
    //      connects and sends its single request.
    //   2. writer.Write() in a loop — each call posts one completion to the CQ.
    //      ok=true: reply was delivered. ok=false: client has disconnected early.
    //   3. writer.Finish() — close the stream; tag fires when acknowledged.
    //
    // The stream of outgoing replies is produced by the concrete server's
    // Generate() method, which returns a CoroStream<ReplyType>. The name
    // "Generate" is used instead of "Handle" because C++ cannot overload on
    // return type — CoroStream<ReplyType> vs Coro<ReplyType> would be ambiguous.
    // -------------------------------------------------------------------------

    template<typename Method>
    static coro::Coro<void> HandleNextRequest(
        RpcTypeServerStreamTag,
        std::shared_ptr<ConcreteServer> self,
        Method method)
    {
        using RequestType = typename ServiceMethodTraits<Method>::RequestType;
        using ReplyType   = typename ServiceMethodTraits<Method>::ReplyType;

        grpc::ServerContext ctx;
        RequestType request;
        grpc::ServerAsyncWriter<ReplyType> writer(&ctx);

        // Register for the next incoming server-streaming call.
        {
            auto [start_tx, start_rx] = coro::oneshot_channel<bool>();
            (self->m_service.*method)(&ctx, &request, &writer,
                self->m_cq.get(), self->m_cq.get(), &start_tx);
            auto start_rx_result = co_await start_rx.recv();
            if (!start_rx_result.has_value() || !start_rx_result.value()) co_return;
        }

        // Post a handler for the next incoming call before processing this one.
        self->m_join_set.spawn(RpcType::ServerStream,
            HandleNextRequest(RpcTypeServerStreamTag{}, self, method));

        // Obtain the reply stream from the concrete server's Generate() method.
        auto reply_stream = self->Generate(request);

        // Write each reply to the client one at a time.
        while (auto item = co_await coro::next(reply_stream)) {
            auto [write_tx, write_rx] = coro::oneshot_channel<bool>();
            writer.Write(*item, &write_tx);
            auto write_result = co_await write_rx.recv();
            if (!write_result.has_value() || !write_result.value()) co_return; // client disconnected
        }

        // All replies sent; close the stream.
        auto [finish_tx, finish_rx] = coro::oneshot_channel<bool>();
        writer.Finish(grpc::Status::OK, &finish_tx);
        (void)co_await finish_rx.recv();
    }

    // -------------------------------------------------------------------------
    // Bidi-streaming RPC handler
    //
    // gRPC async bidi-streaming flow:
    //   1. RequestMethod() — register for the next call; tag fires when a client
    //      connects.
    //   2. stream.Read() and stream.Write() may be outstanding simultaneously.
    //      Read: ok=true: message received; ok=false: client half-closed.
    //      Write: ok=true: reply delivered; ok=false: client disconnected.
    //   3. stream.Finish() — close the server side; tag fires when acknowledged.
    //
    // Client messages are read by `request_stream`, a CoroStream handed to the
    // concrete server's Process() method — a CoroStream<ReplyType> that reads
    // requests and yields replies. The writer loop below writes each reply to the
    // client. The name "Process" avoids both the Handle/Generate overload-by-
    // return-type problem and the Generate(Request&) vs Generate(stream) ambiguity.
    //
    // `request_stream` borrows `stream`, which lives on this coroutine's frame.
    // Process() owns `request_stream`, and `reply_stream` owns Process(), so both
    // are destroyed before `stream` is.
    // -------------------------------------------------------------------------

    template<typename Method>
    static coro::Coro<void> HandleNextRequest(
        RpcTypeBidiStreamTag,
        std::shared_ptr<ConcreteServer> self,
        Method method)
    {
        using RequestType = typename ServiceMethodTraits<Method>::RequestType;
        using ReplyType   = typename ServiceMethodTraits<Method>::ReplyType;

        grpc::ServerContext ctx;
        grpc::ServerAsyncReaderWriter<ReplyType, RequestType> stream(&ctx);

        // Register for the next incoming bidi-streaming call.
        {
            auto [start_tx, start_rx] = coro::oneshot_channel<bool>();
            (self->m_service.*method)(&ctx, &stream,
                self->m_cq.get(), self->m_cq.get(), &start_tx);
            auto start_rx_result = co_await start_rx.recv();
            if (!start_rx_result.has_value() || !start_rx_result.value()) co_return;
        }

        // Post a handler for the next incoming call before processing this one.
        self->m_join_set.spawn(RpcType::BidiStream,
            HandleNextRequest(RpcTypeBidiStreamTag{}, self, method));

        auto request_stream = [](auto self, auto &stream) -> coro::CoroStream<RequestType> {
            while (true) {
                RequestType request;
                auto [read_tx, read_rx] = coro::oneshot_channel<bool>();
                stream.Read(&request, &read_tx);
                auto read_result = co_await read_rx.recv();
                if (!read_result.has_value() || !read_result.value()) co_return; // client half-closed
                co_yield std::move(request);
            }
            co_return;
        }(self, stream);

        // Writer loop: consume replies from Process() and write them to the client.
        auto reply_stream = self->Process(std::move(request_stream));
        bool write_ok = true;
        while (auto item = co_await coro::next(reply_stream)) {
            auto [write_tx, write_rx] = coro::oneshot_channel<bool>();
            stream.Write(*item, &write_tx);
            auto write_result = co_await write_rx.recv();
             if (!write_result.has_value() || !write_result.value()) {
                write_ok = false;
                break; // client disconnected
            }
        }

        if (!write_ok) {
            co_return; // client disconnected; skip Finish
        }

        // All replies sent; close the stream.
        auto [finish_tx, finish_rx] = coro::oneshot_channel<bool>();
        stream.Finish(grpc::Status::OK, &finish_tx);
        (void)co_await finish_rx.recv();
    }

protected:
    template<typename... Methods>
    explicit GrpcServer(Methods... methods) {
        // Capture the method pointers in a lambda so we can register them in
        // Serve() after the server instance is created and we have entered the runtime.
        m_register_handlers_fn = [methods...](auto *self) {
            (self->RegisterRequestType(methods), ...);
        };
    }

    /*!
     * \brief Registers an RPC method to be handled by this server.
     *
     * Must be called before entering the runtime (i.e. before rt.block_on()).
     * The RpcTypeTag is deduced from the method signature via ServiceMethodTraits,
     * and dispatches to the correct HandleNextRequest overload.
     */
    void RegisterRequestType(auto method) {
        using Traits = ServiceMethodTraits<decltype(method)>;
        m_initial_request_handlers.emplace_back(Traits::RPC_TYPE, HandleNextRequest(
            typename Traits::RpcTypeTag{},
            std::static_pointer_cast<ConcreteServer>(this->shared_from_this()),
            method));
    }

public:
    /*!
     * \brief Starts the gRPC server and drives its completion queue.
     *
     * Serves until Ctrl-C or a shutdown request, then shuts down: in-flight RPCs get a
     * second to finish before gRPC cancels them, and Serve() returns once every
     * handler has ended and the completion queue is drained.
     *
     * The CQ loop runs on a blocking thread (via spawn_blocking) so it does not
     * stall the async executor. Each completion posts ok=true/false to the
     * oneshot channel whose sender was registered as the tag for that operation.
     *
     * Note: ok=false is a valid, expected event for streaming reads (end of
     * client stream). The loop therefore forwards ALL completions and only exits
     * when m_cq->Next() returns false (CQ fully drained and shut down).
     */
    static coro::Coro<void> Serve(std::shared_ptr<ConcreteServer> self, uint16_t port) {
        self->m_register_handlers_fn(self.get());
        std::string address = absl::StrFormat("0.0.0.0:%d", port);
        grpc::ServerBuilder builder;
        builder.AddListeningPort(address, grpc::InsecureServerCredentials());
        builder.RegisterService(&self->m_service);
        self->m_cq = builder.AddCompletionQueue();
        std::unique_ptr<grpc::Server> server(builder.BuildAndStart());

        for (auto& [rpc_type, handler] : self->m_initial_request_handlers) {
            self->m_join_set.spawn(rpc_type, std::move(handler));
        }
        self->m_initial_request_handlers.clear();

        auto cq_loop = coro::spawn_blocking([=]() {
            while (true) {
                bool ok = false;
                void* tag = nullptr;
                // Next() returns false only when the CQ is fully drained after shutdown.
                if (!self->m_cq->Next(&tag, &ok)) break;
                // Forward the completion — ok=false is valid for streaming reads.
                auto* tx = static_cast<coro::OneshotSender<bool>*>(tag);
                tx->send(ok);
            }
        });

        // Reap each handler as it finishes, so the set doesn't grow without bound
        // and a handler that threw is logged. The stop futures are created once and
        // lent to each select() with coro::ref(), so they are watched continuously.
        auto interrupted = coro::ctrl_c();
        auto terminated  = coro::shutdown_requested();
        // Held through the shutdown below if a shutdown request stopped us: on Windows
        // the process ends as soon as it is dropped.
        std::optional<coro::ShutdownGuard> guard;
        for (;;) {
            auto sel = co_await coro::select(
                coro::ref(interrupted),
                coro::ref(terminated),
                coro::when(!self->m_join_set.empty(),
                           [&] { return coro::next(self->m_join_set); }));
            if (sel.index() == 0) break;
            if (sel.index() == 1) {
                guard.emplace(std::move(std::get<1>(sel).value));
                break;
            }
            if (auto& done = std::get<2>(sel).value) LogHandlerFailure(*done);
        }

        LOG(INFO) << "Shutting down";
        // Server::Shutdown() blocks until every in-flight RPC has finished, or until
        // the deadline, when it cancels the rest. It runs on a blocking thread so the
        // handlers keep running meanwhile. Each handler then ends on its own: its
        // pending operations complete with ok=false, and a re-armed RequestMethod()
        // fails to start.
        co_await coro::spawn_blocking([&server]() {
            server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        });
        // Wait for every handler to end, without cancelling any (see m_join_set).
        while (auto done = co_await coro::next(self->m_join_set)) LogHandlerFailure(*done);
        // No handler is left to post to the CQ, so it can be shut down and drained.
        self->m_cq->Shutdown();
        co_await std::move(cq_loop);
    }

private:
    // Normal returns need no report: gRPC already has the call's outcome.
    static void LogHandlerFailure(const coro::JoinResult<void, RpcType>& done) {
        if (done.has_value()) return;
        try {
            done.value();
        } catch (const std::exception& e) {
            LOG(ERROR) << RpcTypeName(done.tag()) << " handler failed: " << e.what();
        } catch (...) {
            LOG(ERROR) << RpcTypeName(done.tag()) << " handler failed";
        }
    }

    Service                           m_service;
    std::function<void(ConcreteServer*)> m_register_handlers_fn;
    // One task per registered method waiting for its next call, plus one per call
    // in progress, each tagged with its RPC type. Serve() reaps them.
    //
    // RACE: never cancel a handler, or drop one any other way while it has a gRPC
    // operation outstanding. The operation's tag points at a OneshotSender on the
    // handler's frame, and the CQ thread would send to it after the frame is gone.
    // The same use-after-free follows if Handle(), Generate() or Process() throws
    // while one of the handler's operations is outstanding, or drops a read of its
    // request stream mid-wait (e.g. as the losing branch of a select()).
    coro::JoinSet<void, RpcType>      m_join_set;
    std::unique_ptr<grpc::ServerCompletionQueue> m_cq;
    std::vector<std::pair<RpcType, coro::Coro<void>>> m_initial_request_handlers;
};
