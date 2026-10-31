// Signals on the IoDriver. The handler and self-pipe live behind the seam in
// include/coro/detail/sys/signal.h; this file fans deliveries out to watchers. See
// doc/design/signal_handling.md.

#include <coro/io/signal.h>
#include <coro/detail/rc.h>
#include <coro/detail/socket_state.h>
#include <coro/detail/sys/signal.h>
#include <coro/detail/waker.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <csignal>   // SIGINT, SIGTERM, SIGHUP
#endif

namespace coro {

namespace detail {

namespace {

struct Registry;
Registry& registry();

} // namespace

/**
 * @brief One SignalFuture's or SignalStream's watch on a set of signals.
 *
 * Holds its own dup() of the self-pipe's read end, registered with the creating
 * Runtime's driver. Whichever watcher's task wakes first drains the pipe and calls
 * broadcast(), which hands every watcher in the process its new deliveries; the other
 * watchers find the pipe empty and re-arm. That way a watcher on a Runtime whose
 * driver isn't turning right now still gets its events, through its waker.
 */
struct SignalState {
    SignalState(IoDriver& driver, std::span<const int> signums, const char* what);
    ~SignalState();

    SignalState(const SignalState&)            = delete;
    SignalState& operator=(const SignalState&) = delete;

    /// PollReady once `pending` is non-empty. Only the owning future or stream polls.
    PollResult<void> poll_pending(Context& ctx);

    struct Watch {
        int      signum;
        uint64_t seen;   // sys::signal_count(signum) when last handed out
    };

    const char*        what;
    std::vector<Watch> watches;   // GUARDED BY registry().mutex
    SocketState        pipe;      // this watcher's dup of the self-pipe's read end

    std::mutex              mutex;
    std::deque<SignalEvent> pending;   // GUARDED BY mutex; at most one entry per signum
    Weak<Waker>             waker;     // GUARDED BY mutex; weak: the task owns us
};

namespace {

// Every live SignalState. Lock order: Registry::mutex, then a SignalState::mutex.
struct Registry {
    std::mutex                mutex;
    std::vector<SignalState*> states;   // GUARDED BY mutex
};

Registry& registry() {
    // Leaked on purpose: a watcher on another thread may still be dropped during
    // static destruction at exit.
    static Registry* r = new Registry;
    return *r;
}

/// Hands every watcher the deliveries counted since it last looked, and wakes those
/// that got any. Called after draining the self-pipe, so every delivery whose byte was
/// drained is already counted (the handler counts before it writes).
void broadcast() {
    std::vector<Rc<Waker>> to_wake;
    {
        Registry& reg = registry();
        std::lock_guard lock(reg.mutex);
        for (SignalState* state : reg.states) {
            for (SignalState::Watch& watch : state->watches) {
                const uint64_t now = sys::signal_count(watch.signum);
                if (now == watch.seen) continue;
                const uint64_t delta = now - watch.seen;
                watch.seen = now;

                std::lock_guard state_lock(state->mutex);
                // Coalesce: one pending entry per signum. A linear scan is fine; a
                // watcher watches a handful of signals at most.
                auto it = std::find_if(state->pending.begin(), state->pending.end(),
                    [&](const SignalEvent& e) { return e.signum == watch.signum; });
                if (it != state->pending.end()) it->count += delta;
                else state->pending.push_back(SignalEvent{watch.signum, delta});
                if (auto w = std::exchange(state->waker, Weak<Waker>{}).lock())
                    to_wake.push_back(std::move(w));
            }
        }
    }
    for (auto& w : to_wake) w->wake();
}

} // namespace

SignalState::SignalState(IoDriver& driver, std::span<const int> signums,
                         const char* what_)
    : what(what_), pipe(driver, sys::signal_pipe_dup()) {
    Registry& reg = registry();
    std::lock_guard lock(reg.mutex);
    try {
        for (int signum : signums) {
            const bool duplicate = std::any_of(watches.begin(), watches.end(),
                [&](const Watch& w) { return w.signum == signum; });
            if (duplicate) continue;
            // The baseline is read before the handler is installed, and under the
            // registry mutex so no broadcast runs in between: a delivery from here on
            // counts for this watcher, and none is handed out before it is registered.
            const uint64_t seen = sys::signal_count(signum);
            sys::signal_watch(signum);
            watches.push_back(Watch{signum, seen});
        }
    } catch (...) {
        for (const Watch& w : watches) sys::signal_unwatch(w.signum);
        sys::signal_pipe_release(pipe.fd);
        throw;   // `pipe` is a constructed member, so it deregisters and closes itself
    }
    reg.states.push_back(this);
}

SignalState::~SignalState() {
    Registry& reg = registry();
    std::lock_guard lock(reg.mutex);
    std::erase(reg.states, this);
    for (const Watch& w : watches) sys::signal_unwatch(w.signum);
    sys::signal_pipe_release(pipe.fd);
    // `pipe` deregisters and closes after this body, outside the registry mutex.
}

PollResult<void> SignalState::poll_pending(Context& ctx) {
    for (;;) {
        {
            std::lock_guard lock(mutex);
            if (!pending.empty()) return PollReady;
            // Stored before draining, under the same mutex broadcast() takes: a
            // broadcast from another watcher's task can't slip in between the check
            // and the store, so it always finds this waker.
            waker = ctx.get_weak_waker();
        }
        auto drained = pipe.reg.poll_io(IoDirection::Read, ctx, [this] {
            return sys::signal_pipe_drain(pipe.fd);
        });
        if (!drained) return PollPending;   // empty pipe; driver or broadcast wakes us
        if (!*drained) return PollError(socket_error(drained->error(), what));
        broadcast();   // may have filled `pending`; loop to check
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// SignalStream
// ---------------------------------------------------------------------------

SignalStream::SignalStream(std::unique_ptr<detail::SignalState> state) noexcept
    : m_state(std::move(state)) {}

SignalStream::SignalStream(SignalStream&&) noexcept = default;
SignalStream& SignalStream::operator=(SignalStream&&) noexcept = default;
SignalStream::~SignalStream() = default;

PollResult<std::optional<SignalEvent>> SignalStream::poll_next(detail::Context& ctx) {
    auto ready = m_state->poll_pending(ctx);
    if (ready.isPending()) return PollPending;
    if (ready.isError()) return PollError(ready.error());
    std::lock_guard lock(m_state->mutex);
    SignalEvent event = m_state->pending.front();   // non-empty: only we pop
    m_state->pending.pop_front();
    return std::optional<SignalEvent>(event);
}

// ---------------------------------------------------------------------------
// SignalFuture
// ---------------------------------------------------------------------------

SignalFuture::SignalFuture(std::unique_ptr<detail::SignalState> state) noexcept
    : m_state(std::move(state)) {}

SignalFuture::SignalFuture(SignalFuture&&) noexcept = default;
SignalFuture& SignalFuture::operator=(SignalFuture&&) noexcept = default;
SignalFuture::~SignalFuture() = default;

PollResult<void> SignalFuture::poll(detail::Context& ctx) {
    // Never pops, so once fired it stays ready.
    return m_state->poll_pending(ctx);
}

// ---------------------------------------------------------------------------
// Constructors: signal / signal_stream, ctrl_c, shutdown_requested,
// windows::console_event
// ---------------------------------------------------------------------------

namespace {

std::unique_ptr<detail::SignalState> make_state(std::span<const int> signums,
                                                const char* what) {
    IoDriver& driver = detail::socket_io_driver(what);
    return std::make_unique<detail::SignalState>(driver, signums, what);
}

std::unique_ptr<detail::SignalState> make_state(std::initializer_list<int> signums,
                                                const char* what) {
    return make_state(std::span<const int>(signums.begin(), signums.size()), what);
}

#ifdef _WIN32
constexpr int kCtrlC = static_cast<int>(windows::ConsoleEvent::CtrlC);
#else
constexpr int kCtrlC = SIGINT;
#endif

} // namespace

#ifndef _WIN32
SignalFuture signal(int signum) {
    return SignalFuture(make_state({signum}, "coro::signal"));
}

SignalStream signal_stream(std::initializer_list<int> signums) {
    return SignalStream(make_state(signums, "coro::signal_stream"));
}
#endif

SignalFuture ctrl_c() {
    return SignalFuture(make_state({kCtrlC}, "coro::ctrl_c"));
}

SignalStream ctrl_c_stream() {
    return SignalStream(make_state({kCtrlC}, "coro::ctrl_c_stream"));
}

ShutdownFuture shutdown_requested(ShutdownOptions options) {
#ifdef _WIN32
    (void)options;   // Close is always watched: nothing else holds the process open
    using windows::ConsoleEvent;
    const int signums[] = {static_cast<int>(ConsoleEvent::Close),
                           static_cast<int>(ConsoleEvent::Logoff),
                           static_cast<int>(ConsoleEvent::Shutdown)};
    const std::span<const int> watched(signums);
#else
    int         signums[2]{};
    std::size_t n = 0;
    // A signal ignored now (SIGHUP under nohup) stays ignored: watching it would undo
    // what the user asked for.
    if (!detail::sys::signal_ignored(SIGTERM)) signums[n++] = SIGTERM;
    if (options.hangup && !detail::sys::signal_ignored(SIGHUP)) signums[n++] = SIGHUP;
    const std::span<const int> watched(signums, n);
#endif
    return ShutdownFuture(make_state(watched, "coro::shutdown_requested"));
}

// ---------------------------------------------------------------------------
// ShutdownGuard / ShutdownFuture
// ---------------------------------------------------------------------------

// The guard is one more watch of each signal its future watched, with no fd. On
// Windows the handler waits for an event's watch count to reach 0
// (signal_windows.cpp), so it counts the guard like a watcher; on POSIX the watch
// keeps coro's handler in place, which absorbs a repeated request.
ShutdownGuard::ShutdownGuard(const detail::SignalState& state) {
    std::lock_guard lock(detail::registry().mutex);   // guards `state.watches`
    for (const detail::SignalState::Watch& w : state.watches) {
        // Doesn't throw: `state` already watches it, so this only bumps a count.
        detail::sys::signal_watch(w.signum);
        m_signums[m_count++] = w.signum;
    }
}

ShutdownGuard::ShutdownGuard(ShutdownGuard&& other) noexcept
    : m_signums(other.m_signums), m_count(std::exchange(other.m_count, 0)) {}

ShutdownGuard& ShutdownGuard::operator=(ShutdownGuard&& other) noexcept {
    if (this == &other) return *this;
    release();
    m_signums = other.m_signums;
    m_count   = std::exchange(other.m_count, 0);
    return *this;
}

ShutdownGuard::~ShutdownGuard() { release(); }

void ShutdownGuard::release() noexcept {
    for (std::size_t i = 0; i < m_count; ++i) detail::sys::signal_unwatch(m_signums[i]);
    m_count = 0;
}

ShutdownFuture::ShutdownFuture(std::unique_ptr<detail::SignalState> state) noexcept
    : m_state(std::move(state)) {}

ShutdownFuture::ShutdownFuture(ShutdownFuture&&) noexcept = default;
ShutdownFuture& ShutdownFuture::operator=(ShutdownFuture&&) noexcept = default;
ShutdownFuture::~ShutdownFuture() = default;

PollResult<ShutdownGuard> ShutdownFuture::poll(detail::Context& ctx) {
    auto ready = m_state->poll_pending(ctx);
    if (ready.isPending()) return PollPending;
    if (ready.isError()) return PollError(ready.error());
    // Made while this future still watches, so on Windows the handler's count never
    // reaches 0 between the future letting go and the guard taking over.
    return ShutdownGuard(*m_state);
}

#ifdef _WIN32
namespace windows {

namespace {

// The events that end the process once the handler returns. Watched only through
// shutdown_requested(), whose guard keeps the handler waiting.
bool ends_process(ConsoleEvent event) noexcept {
    return event == ConsoleEvent::Close || event == ConsoleEvent::Logoff ||
           event == ConsoleEvent::Shutdown;
}

void reject_ending_events(std::initializer_list<ConsoleEvent> events, const char* what) {
    for (ConsoleEvent e : events)
        if (ends_process(e))
            throw std::invalid_argument(std::string(what) +
                ": Close, Logoff and Shutdown end the process; watch them with "
                "coro::shutdown_requested()");
}

} // namespace

SignalFuture console_event(ConsoleEvent event) {
    reject_ending_events({event}, "coro::windows::console_event");
    return SignalFuture(make_state({static_cast<int>(event)}, "coro::windows::console_event"));
}

SignalStream console_event_stream(std::initializer_list<ConsoleEvent> events) {
    reject_ending_events(events, "coro::windows::console_event_stream");
    std::vector<int> signums;
    signums.reserve(events.size());
    for (ConsoleEvent e : events) signums.push_back(static_cast<int>(e));
    return SignalStream(make_state(signums, "coro::windows::console_event_stream"));
}

} // namespace windows
#endif

} // namespace coro
