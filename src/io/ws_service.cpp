// LwsService: one lws_context serviced from an IoDriver's turns. See ws_service.h
// and doc/design/websocket_stream.md, "lws on the I/O driver".

#include "ws_service.h"

#include <coro/runtime/io_driver.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace coro::detail::ws {

// lws's sockets go straight to the driver: an int on POSIX, a SOCKET on Windows.
static_assert(std::is_same_v<lws_sockfd_type, sys::RawFd>,
              "lws's socket type must be the sys layer's RawFd");

int protocol_cb(lws* wsi, lws_callback_reasons reason, void* user, void* in, std::size_t len);

namespace {

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

// lws's allocator (lws_set_allocator()): realloc() with a size, free() without one.
// It is lws's own default without the bookkeeping.
//
// Race (the reason): the default also keeps the total it has allocated in one counter
// for the whole process, with no lock. Contexts here are serviced by the threads of
// different Runtimes, and every allocation of theirs raced on that counter. Only
// lws_get_allocated_heap() reads it, so it was harmless, but it was a data race.
void* lws_allocator(void* ptr, std::size_t size, const char* /*reason*/) {
    if (size) return std::realloc(ptr, size);
    std::free(ptr);
    return nullptr;
}

// Once, before the first context is created. Both settings are for the whole process.
void init_lws() {
    static std::once_flag once;
    std::call_once(once, [] {
        // Lines logged off a create() call (from a turn) go nowhere, as with logging
        // off.
        lws_set_log_level(LLL_ERR | LLL_WARN, capture_lws_log);
        // For a program that also uses lws itself: lws_get_allocated_heap() stops
        // counting from here on, and an allocator of its own that it installed
        // earlier is replaced.
        //
        // Potential race: lws reads the allocator with no lock, so this must not
        // overlap another thread's use of lws. None of coro's can, since no context
        // of coro's exists yet. A program's own lws threads could.
        lws_set_allocator(lws_allocator);
    });
}

// The log context of one lws context, with the levels and the sink of
// init_lws(). Each context gets its own (info.log_cx); the context, its
// vhost and its connections then all use it.
//
// Race (the reason): a context given none shares lws's process-wide default, and lws
// counts in it, with no lock, every context, vhost and connection that is created or
// freed. Contexts here are serviced by the threads of different Runtimes, so two of
// them freeing a connection at once raced on that count. With one per context the
// count is only touched by the thread that has the context. Nothing reads the count
// in a release build of lws; a debug build asserts on it.
lws_log_cx_t make_log_cx() {
    lws_log_cx_t cx{};
    cx.u.emit    = capture_lws_log;
    cx.lll_flags = LLL_ERR | LLL_WARN;
    return cx;
}

// What lws_service_adjust_timeout() is asked to shorten: it comes back unchanged
// when lws has no timer pending.
constexpr int kNoLwsTimer = INT_MAX;

// How many times in a row after_lws() services work lws says is already pending
// before it lets the rest of the turn run. See after_lws().
constexpr int kMaxForcedPasses = 4;

// Held across every lws_create_context() and lws_context_destroy().
//
// Race (the reason): a context is otherwise touched by one thread at a time, but
// creating and destroying one also touches state lws keeps for the whole process
// with no lock of its own: the count of contexts that asked for the SSL global init
// (lws 4.5). Contexts here are created and destroyed on the threads of whichever
// Runtimes use them, so two can be in those calls at once.
//
// Taken after the client registry's mutex and never the other way round. lws calls
// back while it is held (the event-loop ops, the protocol callbacks); those only
// record, deregister and wake, and never create or destroy a context.
std::mutex g_lws_lifecycle;

int anchor_cb(lws*, lws_callback_reasons, void*, void*, std::size_t) { return 0; }

// Creates, once per process, an lws context that is never serviced and never
// destroyed, before the first client context.
//
// lws 4.5.3 to 4.5.8 count the contexts created with
// LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT and call OPENSSL_cleanup() when the last one
// is destroyed. OpenSSL can't be initialized again after that: every later digest
// fails, and with it every later client context ("lws_context_init_client_ssl
// failed", nothing in OpenSSL's error queue). Client contexts come and go with
// their Runtimes, so this one holds the count above zero. It costs two descriptors
// and an SSL_CTX with no certificates in it. Other lws versions don't need it and
// aren't harmed by it.
//
// @throws std::system_error if it can't be created; the next call tries again.
void ensure_ssl_anchor() {
    // Kept in a static so that leak checkers see the context as reachable. Volatile
    // because nothing reads it: an optimizing compiler otherwise drops the store, and
    // LeakSanitizer then reports the context. C++ serializes the initialization, and
    // runs it again if it threw.
    static lws_context* volatile anchor = [] {
        // lws keeps the pointer.
        static const lws_protocols protocols[] = {
            {"coro-ws-anchor", anchor_cb, 0, 0, 0, nullptr, 0},
            {nullptr, nullptr, 0, 0, 0, nullptr, 0},
        };
        // lws keeps this pointer too. See make_log_cx().
        static lws_log_cx_t log_cx = make_log_cx();
        lws_context_creation_info info{};
        info.log_cx    = &log_cx;
        info.port      = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols;
        // Without the OS's CA certificates: nothing ever connects through it.
        info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
                         LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS;

        init_lws();
        std::string  errors;
        lws_context* ctx;
        {
            std::lock_guard lifecycle(g_lws_lifecycle);
            t_lws_errors = &errors;
            ctx          = lws_create_context(&info);
            t_lws_errors = nullptr;
        }
        if (!ctx)
            throw std::system_error(EIO, std::generic_category(),
                                    "WsStream::connect: lws_create_context failed (" +
                                        errors + ")");
        return ctx;
    }();
}

} // namespace

// =============================================================================
// Wakers
// =============================================================================

// One registered lws socket: the level-triggered registration and the waker the
// driver wakes, inside its turn, whenever the fd is ready.
//
// When lws lets go of the socket, or the context is destroyed, the Socket is marked
// dead and dropped, so a wake the driver still delivers (its shutdown() collects
// the wakers first, then wakes them) touches nothing but `dead`. `dead` needs no
// lock: it is written and read only inside the driver's turns.
struct LwsService::Socket final : Waker, std::enable_shared_from_this<Socket> {
    Socket(std::weak_ptr<LwsService> service_, lws* wsi_)
        : service(std::move(service_)), wsi(wsi_) {}

    void wake() override {
        if (dead) return;
        // Nothing may escape into the driver's turn. What can throw here is an
        // allocation or the driver refusing a timer; the socket is then serviced
        // again on its next event, since the registration is level-triggered.
        try {
            if (auto s = service.lock()) s->on_socket(*this);
        } catch (...) {
        }
    }
    Rc<Waker> clone() override { return shared_from_this(); }

    std::weak_ptr<LwsService> service;
    lws*                      wsi;
    IoRegistration            registration;
    // The interest the registration currently has.
    bool                      read  = false;
    bool                      write = false;
    bool                      dead  = false;
};

// The waker of the driver timer that stands for lws's next internal timer. Like
// Socket, with the same `dead` rule.
struct LwsService::TimerWaker final : Waker, std::enable_shared_from_this<TimerWaker> {
    explicit TimerWaker(std::weak_ptr<LwsService> service_) : service(std::move(service_)) {}

    void wake() override {
        if (dead) return;
        try {
            if (auto s = service.lock()) s->on_timer();
        } catch (...) {
        }
    }
    Rc<Waker> clone() override { return shared_from_this(); }

    std::weak_ptr<LwsService> service;
    bool                      dead = false;
};

// =============================================================================
// The custom event loop lws is created with
// =============================================================================

// lws calls these, as plain C callbacks, whenever it adds a socket, changes what it
// wants to hear about one, or frees one. No exception may leave them.
//
// The service is found through lws's per-service-thread event-loop storage, which
// init_pt() fills from lws_context_creation_info::foreign_loops. lws runs init_pt()
// before it creates its first socket.
struct LwsService::Ops {
    static LwsService* service_of(lws_context* context) {
        auto** slot = static_cast<LwsService**>(lws_evlib_tsi_to_evlib_pt(context, 0));
        return slot ? *slot : nullptr;
    }

    static int init_pt(lws_context* context, void* loop, int tsi) {
        auto** slot = static_cast<LwsService**>(lws_evlib_tsi_to_evlib_pt(context, tsi));
        if (!slot || !loop) return 1;
        *slot = static_cast<LwsService*>(loop);
        return 0;
    }

    // A new socket: the wake pipe, a listening socket (init_vhost_listen_wsi), an
    // accepted or connecting one (sock_accept). lws follows it with
    // io(LWS_EV_START | LWS_EV_READ). Non-zero fails the socket.
    static int socket_added(lws* wsi) {
        LwsService* s = service_of(lws_get_context(wsi));
        return s ? s->socket_added(wsi) : 1;
    }

    static void io(lws* wsi, unsigned int flags) {
        if (LwsService* s = service_of(lws_get_context(wsi))) s->socket_io(wsi, flags);
    }

    static void destroy_wsi(lws* wsi) {
        if (LwsService* s = service_of(lws_get_context(wsi))) s->socket_freed(wsi);
    }

    static const lws_event_loop_ops kOps;
    static const lws_plugin_evlib_t kEventLoop;
};

// wsi_logical_close is deliberately not set. lws removes a socket from its poll set
// (io with LWS_EV_STOP for both directions) before it closes the fd on every path,
// which is where the registration is dropped; and with wsi_logical_close set but
// destroy_wsi not, lws 4.3 never frees its wake pipe's wsi.
const lws_event_loop_ops LwsService::Ops::kOps = {
    .name                  = "coro",
    .init_vhost_listen_wsi = socket_added,
    .init_pt               = init_pt,
    .sock_accept           = socket_added,
    .io                    = io,
    .destroy_wsi           = destroy_wsi,
    .evlib_size_pt         = sizeof(LwsService*),
};

const lws_plugin_evlib_t LwsService::Ops::kEventLoop = {
    .hdr = {"coro IoDriver event loop", "lws_evlib_plugin", LWS_BUILD_HASH,
            LWS_PLUGIN_API_MAGIC},
    .ops = &kOps,
};

// =============================================================================
// Creation, the client contexts, release
// =============================================================================

LwsService::LwsService(std::shared_ptr<void> keep_alive, std::move_only_function<void()> on_destroyed)
    : m_keep_alive(std::move(keep_alive)), m_on_destroyed(std::move(on_destroyed)) {}

LwsService::~LwsService() {
    // release() destroyed the context, directly or through a turn, or the driver's
    // shutdown did, or creating it failed. A context still attached at static
    // destruction keeps this object alive (m_self) and is never destroyed.
    assert(m_ctx == nullptr);
}

std::shared_ptr<LwsService> LwsService::create(lws_context_creation_info       info,
                                               std::shared_ptr<void>           keep_alive,
                                               std::move_only_function<void()> on_destroyed,
                                               IoDriver&                       driver,
                                               std::string*                    lws_errors) {
    init_lws();
    // Before the context: lws calls the event-loop ops while it creates it, and
    // again while it tears a failed one down.
    // Not make_shared: the constructor is private.
    std::shared_ptr<LwsService> core(
        new LwsService(std::move(keep_alive), std::move(on_destroyed)));

    // Read only during lws_create_context(), which hands the entry to init_pt().
    void* foreign_loops[1] = {core.get()};
    info.event_lib_custom  = &Ops::kEventLoop;
    info.foreign_loops     = foreign_loops;
    // lws keeps the pointer for as long as the context lives, which this object
    // outlives.
    core->m_log_cx         = make_log_cx();
    info.log_cx            = &core->m_log_cx;

    std::string  errors;
    lws_context* ctx;
    {
        std::lock_guard lifecycle(g_lws_lifecycle);
        t_lws_errors = &errors;
        ctx          = lws_create_context(&info);
        t_lws_errors = nullptr;
    }
    if (!ctx) {
        if (lws_errors) *lws_errors = std::move(errors);
        return nullptr;
    }
    {
        std::lock_guard lk(core->m_mutex);
        core->m_ctx = ctx;
    }
    try {
        core->attach(driver);
    } catch (...) {
        release(std::move(core));   // no driver: destroys the context here
        throw;
    }
    return core;
}

std::shared_ptr<LwsService> LwsService::listen(const lws_context_creation_info& info,
                                               std::shared_ptr<void>            keep_alive,
                                               std::move_only_function<void()>  on_destroyed,
                                               IoDriver&                        driver,
                                               std::string*                     lws_errors) {
    auto core = create(info, std::move(keep_alive), std::move(on_destroyed), driver, lws_errors);
    if (!core) return nullptr;
    // What the caller holds is a handle: dropping its last copy does not destroy
    // this object, which a turn may be inside, but asks a turn to destroy the
    // context. The object goes once that turn is done with it.
    LwsService* raw = core.get();
    return std::shared_ptr<LwsService>(
        raw, [core = std::move(core)](LwsService*) mutable { release(std::move(core)); });
}

// The client contexts: one per IoDriver that has connected, from its first connect
// until it shuts down.
struct LwsService::Clients {
    ~Clients() {
        // At static destruction. A context whose Runtime is already gone was
        // destroyed with it and this does nothing. One whose Runtime is still alive
        // is destroyed only if that Runtime turns its driver again.
        for (auto& service : services) release(std::move(service));
    }

    // Guards `services`, and makes finding or creating a driver's context one step:
    // two tasks connecting at once on a new Runtime get the same context.
    std::mutex                               mutex;
    std::vector<std::shared_ptr<LwsService>> services;
};

std::shared_ptr<LwsService> LwsService::client(IoDriver& driver) {
    // In this order: the anchor's create is what makes OpenSSL register its atexit
    // handler, so `clients`, constructed after it, is destroyed before OpenSSL
    // cleans up.
    ensure_ssl_anchor();
    static Clients clients;

    std::lock_guard lk(clients.mutex);
    // The contexts of drivers that have shut down. Before the search: a new driver
    // may have the address of one of those.
    std::erase_if(clients.services, [](const auto& service) { return !service->alive(); });
    for (auto& service : clients.services)
        if (service->attached_to(driver)) return service;

    // lws keeps the pointer, so the array must outlive every client context.
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
    auto service = create(info, nullptr, nullptr, driver, &lws_errors);
    if (!service)
        throw std::system_error(EIO, std::generic_category(),
                                "WsStream::connect: lws_create_context failed (" +
                                    lws_errors + ")");
    clients.services.push_back(service);
    return service;
}

bool LwsService::attached_to(const IoDriver& driver) {
    std::lock_guard lk(m_mutex);
    return m_driver == &driver;
}

bool LwsService::alive() {
    std::lock_guard lk(m_mutex);
    return m_ctx && !m_destroying && !m_destroy_requested;
}

void LwsService::attach(IoDriver& driver) {
    std::shared_ptr<TimerWaker> waker;
    {
        std::lock_guard lk(m_mutex);
        // Before m_driver: from the moment that is set, the turn-only state belongs
        // to the driver's turns.
        waker = m_timer_waker = std::make_shared<TimerWaker>(weak_from_this());
        m_driver = &driver;
    }
    // The sockets are not registered here but by the first turn, which this timer
    // brings about (on_timer()).
    //
    // Race (the reason): IoDriver::shutdown() may run at any time. Registering here
    // could lose to it between a registration being created and its waker being
    // set, and this context would then never hear of the shutdown. A turn and
    // shutdown() exclude each other, and shutdown() wakes this timer if it is still
    // queued, so in-turn attaching always sees one or the other.
    //
    // The timer's id is not kept: m_timer is turn-only state, and the turn may be
    // running on_timer() before add_timer() returns.
    try {
        driver.add_timer(Clock::now(), waker);
    } catch (...) {
        std::lock_guard lk(m_mutex);
        m_driver = nullptr;
        m_timer_waker.reset();
        throw;
    }
}

void LwsService::release(std::shared_ptr<LwsService> core) noexcept {
    LwsService&  self = *core;
    lws_context* ctx  = nullptr;
    {
        std::lock_guard lk(self.m_mutex);
        // Already destroyed, or being destroyed by a turn right now (its driver shut
        // down). That turn holds its own reference to the object.
        if (!self.m_ctx || self.m_destroying) return;
        if (self.m_driver) {
            // A turn may be inside lws at this moment, so the context can't be
            // destroyed here. Ask for it; the wake pipe brings a turn round.
            //
            // If the driver is never turned again without being shut down either,
            // the context is never destroyed.
            self.m_destroy_requested = true;
            self.m_self              = std::move(core);
            lws_cancel_service(self.m_ctx);
            return;
        }
        // No driver (attaching failed), so no turn can reach the context: this
        // thread has it.
        self.m_destroying = true;
        ctx               = self.m_ctx;
    }
    {
        std::lock_guard lifecycle(g_lws_lifecycle);
        lws_context_destroy(ctx);
    }
    self.finish_destroy();
}

// =============================================================================
// Commands
// =============================================================================

void LwsService::post(Command command) {
    std::lock_guard lk(m_mutex);
    if (!m_ctx || m_destroying) return;
    // One wake per batch. A queue that already holds a command has a wake on its
    // way: the post that found it empty sent one, and run_commands() takes the
    // whole queue, under this mutex, each time lws reports a wake. (lws reports it
    // to every protocol of the context, initialized or not.)
    const bool wake = m_commands.empty();
    m_commands.push_back(std::move(command));
    // Makes the wake pipe readable, which stays so until a turn services it: the
    // command is never stranded, whether a thread is blocked in the turn, between
    // turns, or (the pipe not yet registered) before the first one. Called under the
    // mutex so it can't overlap lws_context_destroy(), which only runs once
    // m_destroying is set under the same mutex.
    if (wake) lws_cancel_service(m_ctx);
}

void LwsService::run_commands(lws_context* context) {
    LwsService* self = Ops::service_of(context);
    if (!self) return;
    // Taken out of the object while it is in use, so that a call made from inside a
    // command would find an empty one of its own rather than this batch.
    std::vector<Command> running = std::move(self->m_running);
    {
        std::lock_guard lk(self->m_mutex);
        running.swap(self->m_commands);
    }
    for (auto& command : running) {
        // This runs inside an lws callback: nothing may unwind through lws.
        try {
            command.run(context, command.target.get());
        } catch (...) {
        }
    }
    // Drops the targets, which may free them, and keeps the storage.
    running.clear();
    self->m_running = std::move(running);
}

// =============================================================================
// What lws tells us about its sockets
// =============================================================================

int LwsService::socket_added(lws* wsi) noexcept {
    try {
        // The wsi may be known already: lws gives one a new socket after a failed
        // connect attempt.
        Entry& entry = m_table[wsi];
        const sys::RawFd fd = lws_get_socket_fd(wsi);
        if (entry.socket && entry.fd != fd) drop_registration(entry);
        entry.fd = fd;
        return 0;
    } catch (...) {
        return 1;
    }
}

void LwsService::socket_io(lws* wsi, unsigned int flags) noexcept {
    try {
        const bool start = (flags & LWS_EV_START) != 0;
        if (!start && (flags & LWS_EV_READ) && (flags & LWS_EV_WRITE)) {
            // lws took the socket out of its poll set, which it does before it
            // closes the fd. The registration must go first: see IoRegistration.
            auto it = m_table.find(wsi);
            if (it == m_table.end()) return;
            drop_registration(it->second);
            it->second.read  = false;
            it->second.write = false;
            return;
        }
        Entry& entry = m_table[wsi];
        if (flags & LWS_EV_READ) entry.read = start;
        if (flags & LWS_EV_WRITE) entry.write = start;
        sync_registration(wsi, entry);
    } catch (...) {
        // The driver refused the registration or the interest change, and lws can't
        // be told. The socket goes unserviced until lws's own timeout closes it.
    }
}

void LwsService::socket_freed(lws* wsi) noexcept {
    auto it = m_table.find(wsi);
    if (it == m_table.end()) return;
    drop_registration(it->second);
    m_table.erase(it);
}

void LwsService::sync_registration(lws* wsi, Entry& entry) {
    // Before the first turn after attaching, and while the context is destroyed,
    // sockets are only recorded.
    if (!m_registered) return;

    const sys::RawFd fd = lws_get_socket_fd(wsi);
    if (entry.socket && entry.fd != fd) drop_registration(entry);
    entry.fd = fd;
    // Not `fd < 0`: RawFd is unsigned on Windows, where lws's invalid SOCKET is all ones.
    if (fd == sys::kInvalidFd) return;

    const sys::Interest interest{entry.read, entry.write};
    if (!entry.socket) {
        auto socket = std::make_shared<Socket>(weak_from_this(), wsi);
        socket->registration = IoRegistration(*m_driver, fd, interest, sys::Trigger::Level);
        // In a turn, so no shutdown() can fall between the two calls (see
        // IoRegistration::set_waker()).
        socket->registration.set_waker(socket);
        socket->read  = entry.read;
        socket->write = entry.write;
        entry.socket  = std::move(socket);
    } else if (entry.socket->read != entry.read || entry.socket->write != entry.write) {
        entry.socket->registration.set_interest(interest);
        entry.socket->read  = entry.read;
        entry.socket->write = entry.write;
    }
}

void LwsService::drop_registration(Entry& entry) noexcept {
    if (!entry.socket) return;
    // The Socket may be the one whose wake() is running; the driver holds a
    // reference to it for the duration, and a level-triggered waker may deregister
    // its own registration.
    entry.socket->dead = true;
    entry.socket->registration.deregister();
    entry.socket.reset();
}

// =============================================================================
// Turns
// =============================================================================

void LwsService::on_socket(Socket& socket) {
    if (leave_if_driver_shut_down()) return;

    const IoLevelReady ready = socket.registration.take_ready();
    auto it = m_table.find(socket.wsi);
    if (it == m_table.end() || it->second.socket.get() != &socket) return;
    const Entry& entry = it->second;

    // The driver reports an error or hang-up as both directions. Passing lws a
    // direction it isn't waiting for would only make it call back for nothing, so
    // keep to what it asked for; that still leaves at least one direction in which
    // it will find the error.
    bool readable = ready.readable;
    bool writable = ready.writable;
    if (entry.read || entry.write) {
        readable = readable && entry.read;
        writable = writable && entry.write;
        // A stale event for a direction lws has since stopped: see
        // IoRegistration::set_interest().
        if (!readable && !writable) return;
    }

    // lws acts on the directions set in both fields.
    struct lws_pollfd pfd{};
    pfd.fd      = entry.fd;
    pfd.events  = static_cast<short>((readable ? LWS_POLLIN : 0) | (writable ? LWS_POLLOUT : 0));
    pfd.revents = pfd.events;
    // `socket` and `entry` may be gone when this returns: lws may close the
    // connection, which drops the registration (socket_io()).
    lws_service_fd(m_ctx, &pfd);
    after_lws();
}

void LwsService::on_timer() {
    m_timer.reset();
    if (leave_if_driver_shut_down()) return;

    if (!m_registered) {
        // The first turn after attaching: register what lws has open. For a new
        // context that is its wake pipe and, for a listener, the listening socket.
        m_registered = true;
        for (auto& [wsi, entry] : m_table) {
            try {
                sync_registration(wsi, entry);
            } catch (...) {
                // As in socket_io().
            }
        }
    }
    after_lws();
}

// Run after every entry into lws: lws has no loop of its own here, so whatever it
// would have done before its next poll() is done now.
void LwsService::after_lws() {
    for (int pass = 0;; ++pass) {
        // Runs lws's ripe timers and reports how long until the next one. Zero means
        // "don't wait": either a timer is under a millisecond away, or lws holds
        // work that no socket event will announce (TLS data already decrypted, a
        // connection it paused and can now resume).
        const int ms = lws_service_adjust_timeout(m_ctx, kNoLwsTimer, 0);
        if (ms > 0) {
            if (ms != kNoLwsTimer) arm_timer(Clock::now() + std::chrono::milliseconds(ms));
            break;
        }
        if (pass == kMaxForcedPasses) {
            // Still not idle. Come back in the next turn or two rather than hold
            // this one.
            arm_timer(Clock::now() + std::chrono::milliseconds(1));
            break;
        }
        _lws_plat_service_forced_tsi(m_ctx, 0);
    }

    bool destroy;
    {
        std::lock_guard lk(m_mutex);
        destroy = m_destroy_requested;
    }
    // Here and not where the request is seen: lws_context_destroy() must not be
    // called from inside lws_service_fd(), and this point is always outside it.
    if (destroy) destroy_in_turn();
}

void LwsService::arm_timer(Instant deadline) {
    // An earlier timer is left alone: it fires, after_lws() runs, and the right
    // deadline is set then.
    if (m_timer && m_timer_deadline <= deadline) return;
    disarm_timer();
    m_timer          = m_driver->add_timer(deadline, m_timer_waker);
    m_timer_deadline = deadline;
}

void LwsService::disarm_timer() noexcept {
    if (!m_timer) return;
    m_driver->cancel_timer(*m_timer);
    m_timer.reset();
}

bool LwsService::leave_if_driver_shut_down() {
    // Set before IoDriver::shutdown() wakes anyone, so the wake that brings us here
    // sees it. It may also be seen by an ordinary turn just before shutdown() gets
    // the turn; leaving then is as good.
    if (!m_driver->is_shut_down()) return false;
    destroy_in_turn();
    return true;
}

void LwsService::drop_wakers() noexcept {
    for (auto& [wsi, entry] : m_table) drop_registration(entry);
    disarm_timer();
    if (m_timer_waker) {
        m_timer_waker->dead = true;
        m_timer_waker.reset();
    }
    m_registered = false;
}

// Destroys the context. In a turn, and never from inside lws_service_fd(). The
// caller holds a reference to this object (the waker locked one), since m_self is
// dropped by finish_destroy().
void LwsService::destroy_in_turn() {
    lws_context*         ctx;
    std::vector<Command> dropped;
    {
        std::lock_guard lk(m_mutex);
        if (m_destroying || !m_ctx) return;
        m_destroying = true;   // post() drops from here, and no longer touches m_ctx
        ctx          = m_ctx;
        dropped.swap(m_commands);
    }
    dropped.clear();

    // No new registrations while lws closes everything.
    m_registered = false;
    // Closes every connection and the listening socket. lws reports each connection
    // through the protocol callback (which wakes whoever waits on it), including one
    // it holds without a socket, between two connect attempts; and through the
    // event-loop ops, which drop the registrations before the fds are closed.
    {
        std::lock_guard lifecycle(g_lws_lifecycle);
        lws_context_destroy(ctx);
    }
    finish_destroy();
}

void LwsService::finish_destroy() noexcept {
    drop_wakers();
    m_table.clear();

    std::shared_ptr<LwsService>     self;
    std::shared_ptr<void>           keep_alive;
    std::move_only_function<void()> on_destroyed;
    {
        std::lock_guard lk(m_mutex);
        m_ctx        = nullptr;
        m_driver     = nullptr;
        self         = std::move(m_self);
        keep_alive   = std::move(m_keep_alive);
        on_destroyed = std::move(m_on_destroyed);
    }
    // In a turn, or under whoever dropped a context that never had one: nothing may
    // escape.
    try {
        if (on_destroyed) on_destroyed();
    } catch (...) {
    }
    // The context is gone, so nothing points into what keep_alive held. The caller
    // holds a reference to this object, so dropping `self` here is safe.
}

} // namespace coro::detail::ws
