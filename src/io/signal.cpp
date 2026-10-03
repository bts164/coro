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
#include <utility>
#include <vector>

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
    SignalState(IoDriver& driver, std::initializer_list<int> signums, const char* what);
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

SignalState::SignalState(IoDriver& driver, std::initializer_list<int> signums,
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
        throw;   // `pipe` is a constructed member, so it deregisters and closes itself
    }
    reg.states.push_back(this);
}

SignalState::~SignalState() {
    Registry& reg = registry();
    std::lock_guard lock(reg.mutex);
    std::erase(reg.states, this);
    for (const Watch& w : watches) sys::signal_unwatch(w.signum);
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
// signal / signal_stream
// ---------------------------------------------------------------------------

SignalFuture signal(int signum) {
    IoDriver& driver = detail::socket_io_driver("coro::signal");
    return SignalFuture(std::make_unique<detail::SignalState>(driver, std::initializer_list<int>{signum},
                                                              "coro::signal"));
}

SignalStream signal_stream(std::initializer_list<int> signums) {
    IoDriver& driver = detail::socket_io_driver("coro::signal_stream");
    return SignalStream(std::make_unique<detail::SignalState>(driver, signums,
                                                              "coro::signal_stream"));
}

} // namespace coro
