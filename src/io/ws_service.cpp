// LwsService: one lws_context on its own poll() thread. See ws_service.h and
// doc/design/websocket_stream.md, "Service threads".

#include "ws_service.h"

#include <cerrno>
#include <string_view>
#include <system_error>
#include <utility>

namespace coro::detail::ws {

int protocol_cb(lws* wsi, lws_callback_reasons reason, void* user, void* in, std::size_t len);

namespace {

thread_local bool t_tearing_down = false;

// While create() runs lws_create_context() on this thread, the lws error and warning
// lines it logs; null otherwise, and lines are dropped. lws reports why a context
// failed only through its log.
thread_local std::string* t_lws_errors = nullptr;

void capture_lws_log(int /*level*/, const char* line) {
    if (!t_lws_errors) return;
    std::string_view text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    if (!t_lws_errors->empty()) *t_lws_errors += "; ";
    *t_lws_errors += text;
}

void init_lws_logging() {
    static std::once_flag once;
    // Process-wide. Lines logged off a create() call (the service threads) go nowhere,
    // as with logging off.
    std::call_once(once, [] { lws_set_log_level(LLL_ERR | LLL_WARN, capture_lws_log); });
}

} // namespace

std::shared_ptr<LwsService> LwsService::create(const lws_context_creation_info& info,
                                               std::shared_ptr<void>            keep_alive,
                                               std::string*                     lws_errors) {
    init_lws_logging();
    std::string errors;
    t_lws_errors = &errors;
    lws_context* ctx = lws_create_context(&info);
    t_lws_errors = nullptr;
    if (!ctx) {
        if (lws_errors) *lws_errors = std::move(errors);
        return nullptr;
    }
    // Not make_shared: the constructor is private.
    return std::shared_ptr<LwsService>(new LwsService(ctx, std::move(keep_alive)));
}

std::shared_ptr<LwsService> LwsService::client() {
    // Kept for the life of the process, not recreated when unused. Destroying a
    // client context and creating another made the second one fail in
    // lws_context_init_client_ssl() (seen under ASan in the Ubuntu 24.04 container:
    // its client SSL_CTX setup fails without logging why, likely at the SHA-256
    // config hash). Root cause not found; one context sidesteps it, and an idle
    // service thread costs only a blocked poll().
    //
    // Created by the static's initializer, so its destructor is registered after
    // the atexit handler OpenSSL registers during the first create, and runs before
    // OpenSSL cleans up. If the initializer throws, the next call tries again
    // (C++ retries a function-local static whose initialization threw), and the
    // initialization is serialized across threads.
    static const std::shared_ptr<LwsService> s_client = [] {
        // lws keeps the pointer, so the array must outlive the context.
        static const lws_protocols protocols[] = {
            {"coro-ws", protocol_cb, 0, 4096, 0, nullptr, 0},
            {nullptr, nullptr, 0, 0, 0, nullptr, 0},
        };
        lws_context_creation_info info{};
        info.port      = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols;
        // Client TLS (wss://) needs the global SSL init and a client SSL_CTX on the vhost.
        info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

        std::string lws_errors;
        auto service = create(info, nullptr, &lws_errors);
        if (!service)
            throw std::system_error(EIO, std::system_category(),
                                    "WsStream::connect: lws_create_context failed (" +
                                        lws_errors + ")");
        return service;
    }();
    // A WsStream still alive at static destruction holds its own reference, so the
    // service outlives this one.
    return s_client;
}

LwsService::LwsService(lws_context* ctx, std::shared_ptr<void> keep_alive)
    : m_ctx(ctx), m_keep_alive(std::move(keep_alive)), m_thread([this] { run(); }) {}

LwsService::~LwsService() {
    {
        std::lock_guard lk(m_mutex);
        // Already set if the thread quit on its own after an lws_service() error, in
        // which case the context may be gone and must not be touched.
        if (!m_stopping) {
            m_stopping = true;
            lws_cancel_service(m_ctx);
        }
    }
    m_thread.join();
    // The context is gone, so nothing points into what this kept alive.
    m_keep_alive.reset();
}

void LwsService::post(std::move_only_function<void()> command) {
    std::lock_guard lk(m_mutex);
    // Only after an lws_service() error: the thread has quit, and the context may
    // already be destroyed. The command is dropped unrun.
    if (m_stopping) return;
    m_commands.push_back(std::move(command));
    // Wakes the poll. If the thread is between passes, the cancel is latched and the
    // next lws_service() returns at once, so the command is never stranded. Called
    // under the mutex so it can't overlap the thread's lws_context_destroy(), which
    // only runs once m_stopping is set under the same mutex.
    lws_cancel_service(m_ctx);
}

bool LwsService::tearing_down() noexcept { return t_tearing_down; }

void LwsService::run() {
    for (;;) {
        // One poll() pass: dispatches whatever is ready, then returns. Its timeout
        // argument is ignored; lws sleeps until its next internal timer or a cancel.
        const int r = lws_service(m_ctx, 0);

        std::deque<std::move_only_function<void()>> commands;
        bool stopping;
        {
            std::lock_guard lk(m_mutex);
            std::swap(commands, m_commands);
            // An lws_service() error ends the loop as a stop would; setting the flag
            // here makes later posts drop their commands instead of using the context.
            if (r < 0) m_stopping = true;
            stopping = m_stopping;
        }
        // Run even when stopping: everything posted before the stop (a stream's close,
        // say) still happens, in order, before the context goes.
        for (auto& command : commands) command();
        commands.clear();

        if (stopping) break;
    }
    t_tearing_down = true;
    lws_context_destroy(m_ctx);
    t_tearing_down = false;
}

} // namespace coro::detail::ws
