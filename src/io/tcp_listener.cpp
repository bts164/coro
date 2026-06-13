#include <coro/io/tcp_listener.h>
#include <stdexcept>
#include <system_error>

namespace coro {

namespace {
[[noreturn]] void throw_uv_error(int status, const char* what) {
    throw std::system_error(
        std::error_code(-status, std::system_category()), what);
}
} // namespace

TcpListener::TcpListener(std::shared_ptr<ListenHandle> handle,
                         SingleThreadedUvExecutor* uv_exec)
    : m_handle(std::move(handle)), m_uv_exec(uv_exec) {}

TcpListener::TcpListener(TcpListener&&) noexcept = default;
TcpListener& TcpListener::operator=(TcpListener&&) noexcept = default;

TcpListener::~TcpListener() {
    if (!m_handle) return;
    with_context(*m_uv_exec,
        [](std::shared_ptr<ListenHandle> lh, SingleThreadedUvExecutor* exec) -> Coro<void> {
            lh->closed = true;

            // Unblock any pending accept() call with a cancellation error.
            if (lh->accept_notify) {
                lh->accept_notify->complete(UV_ECANCELED);
                lh->accept_notify = nullptr;
            }

            // Discard accepted-but-not-yet-returned connections. TcpStream
            // destructor schedules async uv_close for each via with_context.
            while (!lh->pending.empty()) {
                auto h = std::move(lh->pending.front());
                lh->pending.pop_front();
                TcpStream discard(std::move(h), exec);
            }

            // Close the server handle. connection_cb will not fire after
            // uv_close is called (handle marked closing). Delete the
            // heap-allocated shared_ptr wrapper stored in server.data before
            // overwriting it — safe because we're on the uv thread and
            // lh->closed = true guards any connection_cb that arrives
            // before this point.
            auto* old_sp = static_cast<std::shared_ptr<ListenHandle>*>(
                               reinterpret_cast<uv_handle_t*>(&lh->server)->data);
            delete old_sp;

            UvCallbackResult<int> result;
            reinterpret_cast<uv_handle_t*>(&lh->server)->data = &result;
            uv_close(reinterpret_cast<uv_handle_t*>(&lh->server),
                [](uv_handle_t* h) {
                    static_cast<UvCallbackResult<int>*>(h->data)->complete(0);
                });
            auto [ignored] = co_await wait(result);
            (void)ignored;
        }(std::move(m_handle), m_uv_exec)
    ).detach();
}

// ---------------------------------------------------------------------------
// bind
// ---------------------------------------------------------------------------

TcpListener::BindFuture TcpListener::bind(std::string host, uint16_t port) {
    auto& exec = current_uv_executor();
    return with_context(exec,
        [](SingleThreadedUvExecutor& exec,
           std::string host, uint16_t port) -> Coro<TcpListener> {

            auto lh = std::make_shared<ListenHandle>();
            uv_tcp_init(exec.loop(), &lh->server);

            // Store a heap-allocated shared_ptr wrapper in server.data so
            // connection_cb always has a valid reference even if TcpListener
            // is destroyed before the callback fires.
            lh->server.data = new std::shared_ptr<ListenHandle>(lh);

            struct sockaddr_in addr;
            if (int r = uv_ip4_addr(host.c_str(), port, &addr); r != 0)
                throw_uv_error(r, "TcpListener::bind");

            if (int r = uv_tcp_bind(&lh->server,
                                    reinterpret_cast<const struct sockaddr*>(&addr), 0);
                    r != 0)
                throw_uv_error(r, "TcpListener::bind");

            if (int r = uv_listen(reinterpret_cast<uv_stream_t*>(&lh->server),
                                  128, connection_cb);
                    r != 0)
                throw_uv_error(r, "TcpListener::bind");

            co_return TcpListener(std::move(lh), &exec);
        }(exec, std::move(host), port)
    );
}

// ---------------------------------------------------------------------------
// accept
// ---------------------------------------------------------------------------

TcpListener::AcceptFuture TcpListener::accept() {
    return with_context(*m_uv_exec,
        [](std::shared_ptr<ListenHandle> lh,
           SingleThreadedUvExecutor* exec) -> Coro<TcpStream> {

            while (true) {
                if (lh->closed)
                    throw std::runtime_error("TcpListener::accept: listener is closed");

                if (!lh->pending.empty()) {
                    auto handle = std::move(lh->pending.front());
                    lh->pending.pop_front();
                    co_return TcpStream(std::move(handle), exec);
                }

                // No connection ready yet — arm the one-shot notifier and suspend.
                // Only one accept() may be in flight at a time.
                UvCallbackResult<int> result;
                lh->accept_notify = &result;
                auto [status] = co_await wait(result);
                lh->accept_notify = nullptr;

                if (status < 0)
                    throw_uv_error(status, "TcpListener::accept");
                // Loop: connection_cb pushed a handle into pending; pop it next.
            }
        }(m_handle, m_uv_exec)
    );
}

// ---------------------------------------------------------------------------
// connection_cb — uv thread only
// ---------------------------------------------------------------------------

void TcpListener::connection_cb(uv_stream_t* server, int status) {
    auto* sp = static_cast<std::shared_ptr<ListenHandle>*>(
                   reinterpret_cast<uv_handle_t*>(server)->data);
    auto& lh = **sp;

    if (lh.closed) return;

    if (status < 0) {
        if (lh.accept_notify) {
            auto* r = lh.accept_notify;
            lh.accept_notify = nullptr;
            r->complete(status);
        }
        return;
    }

    // Allocate a stable-address Handle for the accepted connection.
    auto handle = std::make_shared<TcpStream::Handle>();
    uv_tcp_init(reinterpret_cast<uv_handle_t*>(server)->loop, &handle->handle);
    uv_accept(server, reinterpret_cast<uv_stream_t*>(&handle->handle));

    lh.pending.push_back(std::move(handle));

    if (lh.accept_notify) {
        auto* r = lh.accept_notify;
        lh.accept_notify = nullptr;
        r->complete(0);
    }
}

} // namespace coro
