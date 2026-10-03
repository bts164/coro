#pragma once

// Private to src/io: one libwebsockets context plus the thread that services it.
// WsStream and WsListener post every lws call here; nothing else touches lws. See
// doc/design/websocket_stream.md, "Service threads".

#include <coro/detail/rc.h>
#include <coro/detail/waker.h>
#include <coro/io/socket_address.h>
#include <libwebsockets.h>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace coro::detail::ws {

/**
 * @brief Owns an lws_context and a thread running its default poll() loop.
 *
 * The thread alternates `lws_service()` (one poll pass) with the commands posted to
 * it. `post()` is the only way in from other threads: it queues the command and
 * calls `lws_cancel_service()`, lws's one thread-safe entry point, which wakes the
 * poll.
 *
 * Held by `shared_ptr` from WsStream, WsListener and their futures only, never from
 * the state the lws callbacks or commands touch. So the last reference is always
 * dropped off the service thread, and the destructor can join it.
 */
class LwsService {
public:
    /// Creates the context on the calling thread, so bind errors surface here, then
    /// starts the service thread. Returns null if lws_create_context() fails (errno
    /// is not meaningful: lws doesn't preserve it); `lws_errors`, if given, then gets
    /// the error and warning lines lws logged during the attempt, "; "-separated.
    /// `keep_alive` holds whatever `info` points into (protocols, context user data);
    /// it's released after the context is destroyed.
    static std::shared_ptr<LwsService> create(const lws_context_creation_info& info,
                                              std::shared_ptr<void>            keep_alive,
                                              std::string* lws_errors = nullptr);

    /// The process-wide client context every WsStream::connect() shares. Created on
    /// first use and kept until static destruction: lws can't set up client TLS
    /// again once a client context has been destroyed (see ws_service.cpp).
    /// @throws std::system_error if the context can't be created; the next call
    ///         tries again.
    static std::shared_ptr<LwsService> client();

    /// Stops the service thread and destroys the context, closing every connection
    /// still on it. Blocks for the join, which is short: the thread only has to
    /// finish its current pass.
    ~LwsService();

    LwsService(const LwsService&)            = delete;
    LwsService& operator=(const LwsService&) = delete;

    /// Runs `command` on the service thread, in posting order. Safe from any thread.
    /// Commands may call lws; they must not capture a shared_ptr<LwsService>.
    void post(std::move_only_function<void()> command);

    /// The context. Pass it to lws only from the service thread.
    lws_context* context() const noexcept { return m_ctx; }

    /// True on a service thread while it destroys its context. lws fires CLOSED for
    /// every connection then; no stream or future can be waiting by that point (they
    /// hold the service), and the tasks that owned them may be going down with their
    /// executor, so callbacks skip wakes.
    static bool tearing_down() noexcept;

private:
    LwsService(lws_context* ctx, std::shared_ptr<void> keep_alive);
    void run();

    lws_context*          m_ctx;
    std::shared_ptr<void> m_keep_alive;

    // Guards m_commands and m_stopping, and is held across lws_cancel_service() so a
    // cancel never overlaps the context being destroyed.
    std::mutex                                     m_mutex;
    std::deque<std::move_only_function<void()>>    m_commands;
    bool                                           m_stopping = false;

    std::thread m_thread;   // started last, once everything it reads is set
};

/// The address as lws wants it in a connect or bind: numeric text, no port.
std::string numeric_host(const SocketAddress& addr);

/// Fires a waker stored by a WS future, from an lws callback.
///
/// Race (known, shared with the blocking pool's wakes): lock() can succeed just as
/// the task's executor is being destroyed, and the wake then reaches a dying
/// executor. The executor drops its tasks first, so the window is the few
/// instructions between the lock and the wake, and only on Runtime shutdown with a
/// connection still active.
inline void wake(const Weak<Waker>& waker) {
    if (LwsService::tearing_down()) return;
    if (auto w = waker.lock()) w->wake();
}

} // namespace coro::detail::ws
