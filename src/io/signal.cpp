#include <coro/io/signal.h>
#include <coro/coro.h>
#include <coro/task/spawn_on.h>
#include <algorithm>
#include <system_error>
#include <utility>

namespace coro {

namespace detail {

void signal_cb(uv_signal_t* handle, int signum) {
    auto* sp = static_cast<std::shared_ptr<SignalState>*>(
                   reinterpret_cast<uv_handle_t*>(handle)->data);
    auto& state = **sp;

    coro::detail::Rc<coro::detail::Waker> waker;
    {
        std::lock_guard lock(state.mutex);
        if (state.closed) return;

        // Coalesce: if signum already has an unconsumed entry, bump its count in
        // place instead of queuing a second entry. Linear scan is fine — the number
        // of distinct watched signals is always small (a handful at most).
        auto it = std::find_if(state.pending.begin(), state.pending.end(),
                                [signum](const SignalEvent& e) { return e.signum == signum; });
        if (it != state.pending.end()) {
            ++it->count;
        } else {
            state.pending.push_back(SignalEvent{signum, 1});
        }
        waker = std::exchange(state.waker, nullptr);
    }
    if (waker) waker->wake();
}

namespace {

// Shared teardown logic for SignalFuture and SignalStream: marks the state closed,
// wakes any parked waker (poll()/poll_next() observe closed on the next call), then
// stops and closes every watched handle. Runs on the uv thread via with_context.
//
// RACE CONDITION NOTE: closed is set under the same mutex signal_cb checks at its
// top, and uv_signal_stop/uv_close also run on the uv thread (the same thread
// signal_cb runs on) — so there is no window where signal_cb can observe a handle
// after this coroutine has started closing it.
Coro<void> teardown(std::shared_ptr<SignalState> state) {
    coro::detail::Rc<coro::detail::Waker> waker;
    {
        std::lock_guard lock(state->mutex);
        state->closed = true;
        waker = std::exchange(state->waker, nullptr);
    }
    if (waker) waker->wake();

    for (auto& h : state->handles) {
        auto* old_sp = static_cast<std::shared_ptr<SignalState>*>(
                           reinterpret_cast<uv_handle_t*>(&h)->data);
        delete old_sp;
        uv_signal_stop(&h);
        uv_close(reinterpret_cast<uv_handle_t*>(&h), [](uv_handle_t*) {});
    }
    co_return;
}

} // namespace

} // namespace detail

// ---------------------------------------------------------------------------
// SignalStream
// ---------------------------------------------------------------------------

SignalStream::SignalStream(std::shared_ptr<detail::SignalState> state,
                           SingleThreadedUvExecutor* uv_exec) noexcept
    : m_state(std::move(state)), m_uv_exec(uv_exec) {}

SignalStream::SignalStream(SignalStream&&) noexcept = default;
SignalStream& SignalStream::operator=(SignalStream&&) noexcept = default;

SignalStream::~SignalStream() {
    if (!m_state) return;
    with_context(*m_uv_exec, detail::teardown(std::move(m_state))).detach();
}

PollResult<std::optional<SignalEvent>> SignalStream::poll_next(detail::Context& ctx) {
    std::lock_guard lock(m_state->mutex);
    if (m_state->setup_error != 0) {
        return PollError(std::make_exception_ptr(std::system_error(
            std::error_code(-m_state->setup_error, std::system_category()),
            "coro::signal_stream: uv_signal_start failed")));
    }
    if (!m_state->pending.empty()) {
        SignalEvent event = m_state->pending.front();
        m_state->pending.pop_front();
        return std::optional<SignalEvent>(event);
    }
    if (m_state->closed) {
        return std::optional<SignalEvent>(std::nullopt);
    }
    m_state->waker = ctx.getWaker();
    return PollPending;
}

// ---------------------------------------------------------------------------
// SignalFuture
// ---------------------------------------------------------------------------

SignalFuture::SignalFuture(std::shared_ptr<detail::SignalState> state,
                          SingleThreadedUvExecutor* uv_exec) noexcept
    : m_state(std::move(state)), m_uv_exec(uv_exec) {}

SignalFuture::SignalFuture(SignalFuture&&) noexcept = default;
SignalFuture& SignalFuture::operator=(SignalFuture&&) noexcept = default;

SignalFuture::~SignalFuture() {
    if (!m_state) return;
    with_context(*m_uv_exec, detail::teardown(std::move(m_state))).detach();
}

PollResult<void> SignalFuture::poll(detail::Context& ctx) {
    std::lock_guard lock(m_state->mutex);
    if (m_state->setup_error != 0) {
        return PollError(std::make_exception_ptr(std::system_error(
            std::error_code(-m_state->setup_error, std::system_category()),
            "coro::signal: uv_signal_start failed")));
    }
    if (!m_state->pending.empty()) {
        return PollReady;
    }
    m_state->waker = ctx.getWaker();
    return PollPending;
}

// ---------------------------------------------------------------------------
// signal / signal_stream
// ---------------------------------------------------------------------------

namespace {

// Registers one uv_signal_t per requested signum, all sharing signal_cb and `state`.
// Stops at the first failing uv_signal_start (rare — only on an invalid signum) and
// records the error rather than throwing, since this runs detached: nobody is
// co_awaiting it directly, so an exception here would have nowhere to propagate to.
// poll()/poll_next() surface the error to the consumer on their first call instead.
Coro<void> register_handles(std::shared_ptr<detail::SignalState> state,
                            SingleThreadedUvExecutor& exec,
                            std::vector<int> signums) {
    for (int signum : signums) {
        state->handles.emplace_back();
        uv_signal_t& h = state->handles.back();
        uv_signal_init(exec.loop(), &h);
        h.data = new std::shared_ptr<detail::SignalState>(state);

        if (int r = uv_signal_start(&h, detail::signal_cb, signum); r != 0) {
            coro::detail::Rc<coro::detail::Waker> waker;
            {
                std::lock_guard lock(state->mutex);
                state->setup_error = r;
                waker = std::exchange(state->waker, nullptr);
            }
            if (waker) waker->wake();
            break;
        }
    }
    co_return;
}

} // namespace

SignalFuture signal(int signum) {
    auto state = std::make_shared<detail::SignalState>();
    auto& exec = current_uv_executor();
    with_context(exec, register_handles(state, exec, std::vector<int>{signum})).detach();
    return SignalFuture(std::move(state), &exec);
}

SignalStream signal_stream(std::initializer_list<int> signums) {
    auto state = std::make_shared<detail::SignalState>();
    auto& exec = current_uv_executor();
    with_context(exec, register_handles(state, exec, std::vector<int>(signums))).detach();
    return SignalStream(std::move(state), &exec);
}

} // namespace coro
