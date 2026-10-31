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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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
    /// `run(context, target.get())` is called in a turn of the driver the context
    /// is attached to. `target` is what the command acts on, kept alive until the
    /// command has run or been dropped.
    ///
    /// A function pointer and a pointer, not a std::function: posting one never
    /// allocates, whatever it carries. A command that needs more than one object
    /// puts them in a struct of its own and passes that as the target.
    struct Command {
        void (*run)(lws_context* context, void* target);
        std::shared_ptr<void> target;
    };

    /// Creates a listening context on the calling thread, so bind errors surface
    /// here, and attaches it to `driver`. Returns null if lws_create_context() fails
    /// (errno is not meaningful: lws doesn't preserve it); `lws_errors`, if given,
    /// then gets the error and warning lines lws logged during the attempt,
    /// "; "-separated. `keep_alive` holds whatever `info` points into (protocols,
    /// context user data); it's released after the context is destroyed.
    ///
    /// The context is destroyed, closing the listening socket and every connection,
    /// by the first turn after the last copy of the returned pointer is dropped, or
    /// when `driver` shuts down, whichever is first. `on_destroyed` runs right after,
    /// on the thread that destroyed it, whichever way it went; it must not throw or
    /// block. lws tells the protocol callback about the connections it closes, but
    /// not always that the context itself is going, so this is the one place to
    /// learn that.
    ///
    /// @throws std::runtime_error if `driver` has been shut down.
    static std::shared_ptr<LwsService> listen(const lws_context_creation_info& info,
                                              std::shared_ptr<void>            keep_alive,
                                              std::move_only_function<void()>  on_destroyed,
                                              IoDriver&                        driver,
                                              std::string* lws_errors = nullptr);

    /// The client context of `driver`, which every WsStream::connect() on that
    /// driver's Runtime shares. Created by the first call for the driver, and
    /// destroyed, closing every connection, when the driver shuts down. Not when the
    /// last stream drops it: a client context is costly to create (lws loads the
    /// system's CA certificates into each).
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
    /// Dropped unrun if the context is gone (its driver shut down, or the last
    /// handle was dropped): every connection was closed then, so there is nothing
    /// left to act on.
    void post(Command command);

    /// Runs the commands posted to `context`'s service. For the protocol callbacks,
    /// on LWS_CALLBACK_EVENT_WAIT_CANCELLED.
    static void run_commands(lws_context* context);

private:
    struct Socket;
    struct TimerWaker;
    struct Ops;
    struct Clients;

    // One per lws socket (wsi), from the first time lws mentions it until lws frees
    // the wsi. `socket` is set while the fd is registered with the driver.
    struct Entry {
        sys::RawFd              fd    = sys::kInvalidFd;
        bool                    read  = false;
        bool                    write = false;
        std::shared_ptr<Socket> socket;
    };

    LwsService(std::shared_ptr<void> keep_alive, std::move_only_function<void()> on_destroyed);

    // Creates the context and makes `driver` the one that services it. Null if
    // lws_create_context() fails.
    // @throws std::runtime_error if `driver` has been shut down.
    static std::shared_ptr<LwsService> create(lws_context_creation_info       info,
                                              std::shared_ptr<void>           keep_alive,
                                              std::move_only_function<void()> on_destroyed,
                                              IoDriver&                       driver,
                                              std::string*                    lws_errors);

    // @throws std::runtime_error if `driver` has been shut down.
    void attach(IoDriver& driver);
    bool attached_to(const IoDriver& driver);
    // False once the context is destroyed or about to be.
    bool alive();

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
    void drop_wakers() noexcept;
    // The context is gone: lets go of what it pointed into, and runs m_on_destroyed.
    void finish_destroy() noexcept;

    // Guards the members down to m_on_destroyed. Held across lws_cancel_service(), so a
    // cancel never overlaps the context being destroyed.
    //
    // m_ctx and m_driver are written under it, and only by the thread that owns the
    // turn-only state below; that thread therefore reads them without it.
    std::mutex            m_mutex;
    lws_context*          m_ctx    = nullptr;
    // The driver whose turns service the context. Null until attached, and again
    // once the context is destroyed.
    IoDriver*             m_driver = nullptr;
    std::vector<Command>  m_commands;
    // Set by release(): the next turn destroys the context.
    bool                  m_destroy_requested = false;
    // Set once lws_context_destroy() is about to run; post() drops from then on.
    bool                  m_destroying = false;
    // Keeps this object alive from release() until the context is destroyed.
    std::shared_ptr<LwsService> m_self;
    std::shared_ptr<void> m_keep_alive;
    std::move_only_function<void()> m_on_destroyed;

    // --- Turn-only state. Touched only inside a turn of m_driver, or, while
    // --- m_driver is null, by the one thread creating, attaching or destroying the
    // --- context. Hand-over between the two goes through m_mutex (m_driver is set
    // --- and cleared under it), which orders the accesses.
    //
    // True once the first turn after attaching has registered the sockets.
    bool                              m_registered = false;
    std::unordered_map<lws*, Entry>   m_table;
    // The waker of the one driver timer that stands for lws's next internal timer.
    std::shared_ptr<TimerWaker>       m_timer_waker;
    std::optional<TimerId>            m_timer;
    Instant                           m_timer_deadline{};
    // Empty between calls of run_commands(), which swaps it with m_commands: the two
    // trade their storage back and forth, so a batch allocates nothing once both
    // have grown.
    std::vector<Command>              m_running;

    // The context's own log context, so that it shares none with another context.
    // Set before the context is created and only read by coro afterwards; lws counts
    // in it, on whichever thread has the context. See make_log_cx().
    lws_log_cx_t                      m_log_cx{};
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
