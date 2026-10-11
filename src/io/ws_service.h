#pragma once

// Private to src/io: one libwebsockets context serviced by a Runtime's IoDriver.
// WsStream and WsListener post every lws call here; nothing else touches lws. See
// doc/design/websocket_stream.md, "lws on the I/O driver".

#include <coro/detail/rc.h>
#include <coro/detail/sys/poller.h>
#include <coro/detail/timer_queue.h>
#include <coro/detail/waker.h>
#include <coro/io/socket_address.h>
#include <coro/runtime/clock.h>
#include <libwebsockets.h>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace coro {
class IoDriver;
}

namespace coro::detail::ws {

/**
 * @brief Owns an lws_context and services it from an IoDriver's turns.
 *
 * lws has no thread here. The context is created with a custom event loop
 * (`lws_context_creation_info::event_lib_custom`), through which lws reports every
 * socket it adds, removes, or changes its interest in. Each socket becomes a
 * level-triggered IoRegistration whose waker calls `lws_service_fd()` inline, inside
 * the driver's turn. lws's timers ride on one driver timer.
 *
 * So every lws call for a context is made inside a turn of the driver it is attached
 * to, and the turn is lws's mutual exclusion: at most one thread turns a driver at a
 * time. `post()` is the only way in from elsewhere: it queues a command and calls
 * `lws_cancel_service()`, lws's one thread-safe entry point, which makes lws's wake
 * pipe readable. The next turn services the pipe, lws reports
 * `LWS_CALLBACK_EVENT_WAIT_CANCELLED`, and the callback runs the queue
 * (`run_commands()`).
 *
 * Nothing here blocks: the last handle may be dropped on any thread, including
 * inside a turn, and the context is then destroyed by a later turn.
 */
class LwsService : public std::enable_shared_from_this<LwsService> {
public:
    /// Runs in a turn of the driver the context is attached to, with the context.
    using Command = std::move_only_function<void(lws_context*)>;

    /// Creates a listening context on the calling thread, so bind errors surface
    /// here, and attaches it to `driver`. Returns null if lws_create_context() fails
    /// (errno is not meaningful: lws doesn't preserve it); `lws_errors`, if given,
    /// then gets the error and warning lines lws logged during the attempt,
    /// "; "-separated. `keep_alive` holds whatever `info` points into (protocols,
    /// context user data); it's released after the context is destroyed.
    ///
    /// The context is destroyed, closing the listening socket and every connection,
    /// by the first turn after the last copy of the returned pointer is dropped, or
    /// when `driver` shuts down, whichever is first.
    ///
    /// @throws std::runtime_error if `driver` has been shut down.
    static std::shared_ptr<LwsService> listen(const lws_context_creation_info& info,
                                              std::shared_ptr<void>            keep_alive,
                                              IoDriver&                        driver,
                                              std::string* lws_errors = nullptr);

    /// The client context attached to `driver`, which every WsStream::connect() on
    /// that driver's Runtime shares. Client contexts are pooled for the life of the
    /// process: one whose driver has shut down is handed to the next driver that
    /// asks, and a new one is created only when none is free (see ws_service.cpp).
    ///
    /// @throws std::system_error if a context can't be created; the next call tries
    ///         again.
    /// @throws std::runtime_error if `driver` has been shut down.
    static std::shared_ptr<LwsService> client(IoDriver& driver);

    ~LwsService();

    LwsService(const LwsService&)            = delete;
    LwsService& operator=(const LwsService&) = delete;

    /// Runs `command` in a turn, in posting order. Safe from any thread, including
    /// from inside a turn. The command may call lws.
    ///
    /// Dropped unrun if the context is gone or has no driver (its driver shut down):
    /// every connection was closed then, so there is nothing left to act on.
    void post(Command command);

    /// Runs the commands posted to `context`'s service. For the protocol callbacks,
    /// on LWS_CALLBACK_EVENT_WAIT_CANCELLED.
    static void run_commands(lws_context* context);

private:
    enum class Kind { Listener, Client };

    struct Socket;
    struct TimerWaker;
    struct Ops;
    struct ClientPool;

    // One per lws socket (wsi), from the first time lws mentions it until lws frees
    // the wsi. `socket` is set while the fd is registered with the driver.
    struct Entry {
        sys::RawFd              fd        = -1;
        bool                    read      = false;
        bool                    write     = false;
        // Belongs to the context rather than to a connection (the wake pipe, the
        // listening socket): it stays when a client context's driver shuts down.
        bool                    permanent = false;
        std::shared_ptr<Socket> socket;
    };

    explicit LwsService(Kind kind, std::shared_ptr<void> keep_alive);

    static std::shared_ptr<LwsService> create(lws_context_creation_info info, Kind kind,
                                              std::shared_ptr<void> keep_alive,
                                              std::string*          lws_errors);

    // Makes `driver` the one that services this context, if it has none.
    // @throws std::runtime_error if `driver` has been shut down.
    bool try_attach(IoDriver& driver);
    bool attached_to(const IoDriver& driver);

    // Asks for the context to be destroyed, now that nothing holds `core` but this.
    static void release(std::shared_ptr<LwsService> core) noexcept;

    // --- Called by lws through the event-loop ops, and by the wakers. All in a turn,
    // --- or on the one thread that has the context while it has no driver.
    int  socket_added(lws* wsi) noexcept;
    void socket_io(lws* wsi, unsigned int flags) noexcept;
    void socket_freed(lws* wsi) noexcept;
    void sync_registration(lws* wsi, Entry& entry);
    void drop_registration(Entry& entry) noexcept;

    void on_socket(Socket& socket);
    void on_timer();
    void after_lws();
    void arm_timer(Instant deadline);
    void disarm_timer() noexcept;
    bool leave_if_driver_shut_down();
    void destroy_in_turn();
    void detach_in_turn();
    void drop_wakers() noexcept;

    const Kind m_kind;

    // Guards the members down to m_keep_alive. Held across lws_cancel_service(), so a
    // cancel never overlaps the context being destroyed.
    //
    // m_ctx and m_driver are written under it, and only by the thread that owns the
    // turn-only state below; that thread therefore reads them without it.
    std::mutex            m_mutex;
    lws_context*          m_ctx    = nullptr;
    // The driver whose turns service the context. Null until attached, and again
    // once that driver has shut down.
    IoDriver*             m_driver = nullptr;
    std::deque<Command>   m_commands;
    // Set by release(): the next turn destroys the context.
    bool                  m_destroy_requested = false;
    // Set once lws_context_destroy() is about to run; post() drops from then on.
    bool                  m_destroying = false;
    // Keeps this object alive from release() until the context is destroyed.
    std::shared_ptr<LwsService> m_self;
    std::shared_ptr<void> m_keep_alive;

    // --- Turn-only state. Touched only inside a turn of m_driver, or, while
    // --- m_driver is null, by the one thread creating, attaching or destroying the
    // --- context. Hand-over between the two goes through m_mutex (m_driver is set
    // --- and cleared under it), which orders the accesses.
    //
    // True during lws_create_context(): sockets added then are permanent.
    bool                              m_creating = false;
    // True once the first turn after attaching has registered the sockets.
    bool                              m_registered = false;
    std::unordered_map<lws*, Entry>   m_table;
    // The waker of the one driver timer that stands for lws's next internal timer.
    // A new one per attachment.
    std::shared_ptr<TimerWaker>       m_timer_waker;
    std::optional<TimerId>            m_timer;
    Instant                           m_timer_deadline{};
};

/// The address as lws wants it in a connect or bind: numeric text, no port.
std::string numeric_host(const SocketAddress& addr);

/// Fires a waker stored by a WS future, from an lws callback.
///
/// Race (known, shared with the driver's own wakes): lock() can succeed just as
/// the task's executor is being destroyed, and the wake then reaches a dying
/// executor. The executor drops its tasks first, so the window is the few
/// instructions between the lock and the wake, and only for a task of another
/// Runtime than the one servicing the connection.
inline void wake(const Weak<Waker>& waker) {
    if (auto w = waker.lock()) w->wake();
}

} // namespace coro::detail::ws
