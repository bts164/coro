#pragma once

// Cooperative, stackful execution contexts ("fibers") — see
// doc/design/fiber.md. Lets a blocking, callback-driven third-party C
// library suspend from *anywhere in its call stack* and be resumed later,
// without any co_await point inside it. Modeled on spawn_blocking() (see
// spawn_blocking.h) but running on the executor's own thread via a context
// switch instead of a worker thread.

#include <coro/detail/context.h>
#include <coro/detail/fiber_context.h>
#include <coro/detail/poll_result.h>
#include <coro/future.h>
#include <coro/runtime/runtime.h>
#include <coro/task/join_handle.h>

#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace coro {

namespace detail {

// Bridges fiber code (running on its own stack, possibly many ordinary
// non-coroutine frames deep inside a third-party library) back to the
// executor's Context/Waker machinery. Saved/restored around every
// switch_context() call in FiberFuture<T>::poll(), the same pattern
// coro_scope.h's t_current_coro already uses to expose "the currently
// running task" to code that isn't lexically inside it.
//
// Plain globals under CORO_PICO, not thread_local: Cortex-M0+ has no
// hardware TLS, and thread_local compiles down to a call to
// __aeabi_read_tp, which bare-metal newlib doesn't implement -- undefined
// reference at link time. A plain global is equivalent on the
// single-threaded CurrentThreadExecutor -- see coro_scope.h's
// t_current_coro for the same fix applied earlier.
#ifdef CORO_PICO
extern Context*      t_current_fiber_ctx;
extern FiberContext* t_current_fiber_running_ctx; // the fiber's own context
extern FiberContext* t_current_fiber_caller_ctx;  // who to switch back to
#else
extern thread_local Context*      t_current_fiber_ctx;
extern thread_local FiberContext* t_current_fiber_running_ctx; // the fiber's own context
extern thread_local FiberContext* t_current_fiber_caller_ctx;  // who to switch back to
#endif

// Set immediately before a fiber's very first switch_context() (poll()'s
// first call), read exactly once by FiberFuture<T>::trampoline() as it
// starts running on the fresh stack. makecontext() only supports passing
// plain int arguments portably, so the FiberFuture<T>* is threaded through
// here instead, the same way the three thread-locals above thread state
// through switch_context()'s otherwise argument-less transfer.
#ifdef CORO_PICO
extern void* t_fiber_start_arg;
#else
extern thread_local void* t_fiber_start_arg;
#endif

} // namespace detail

/**
 * @brief `Future<T>` whose `poll()` implementation is a context switch instead of
 * resumed compiler-generated coroutine state — see doc/design/fiber.md's
 * "FiberFuture<T>" section.
 *
 * Deliberately not the type users spawn directly: treating a bare
 * `FiberFuture<T>` as freely `select()`-able/droppable like an ordinary
 * `Coro<T>` reintroduces the cancellation problem `spawn_fiber()` (below)
 * exists to avoid. `FiberFuture<T>` stays an internal building block.
 *
 * Move-only, and only safe to move *before* the first `poll()` call (i.e.
 * before the fiber has actually started running) — `coro::spawn()` moves its
 * argument into place exactly once, before ever calling `poll()`, so this
 * matches how it's actually used.
 */
template<typename T>
class FiberFuture {
    // std::monostate placeholder for T = void, since std::optional<void> is
    // ill-formed — same shape as detail::StreamItem in stream.h.
    using ResultStorage = std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>>;

public:
    using OutputType = T;

    explicit FiberFuture(std::function<T()> entry, size_t stack_size)
        : m_entry(std::move(entry)), m_stack_size(stack_size) {}

    FiberFuture(const FiberFuture&)            = delete;
    FiberFuture& operator=(const FiberFuture&) = delete;
    FiberFuture(FiberFuture&&) noexcept            = default;
    FiberFuture& operator=(FiberFuture&&) noexcept = default;

    // m_fiber_ctx's own destructor frees its stack (RAII) -- nothing to do here.
    ~FiberFuture() = default;

    PollResult<T> poll(detail::Context& ctx) {
        if (!m_started) {
            detail::alloc_fiber_stack(m_fiber_ctx, m_stack_size);
            detail::init_fiber_entry(m_fiber_ctx, &FiberFuture::trampoline);
            m_started = true;
        }

        auto* prev_ctx     = detail::t_current_fiber_ctx;
        auto* prev_running = detail::t_current_fiber_running_ctx;
        auto* prev_caller  = detail::t_current_fiber_caller_ctx;
        detail::t_current_fiber_ctx         = &ctx;
        detail::t_current_fiber_running_ctx = &m_fiber_ctx;
        detail::t_current_fiber_caller_ctx  = &m_caller_ctx;
        detail::t_fiber_start_arg           = this;

        detail::switch_context(&m_caller_ctx, &m_fiber_ctx); // resume (or start) the fiber

        detail::t_current_fiber_ctx         = prev_ctx;
        detail::t_current_fiber_running_ctx = prev_running;
        detail::t_current_fiber_caller_ctx  = prev_caller;

        if (!m_finished) return PollPending;
        if (m_exception) return PollError(m_exception);
        if constexpr (std::is_void_v<T>) return PollReady;
        else return std::move(*m_result);
    }

private:
    // Runs on the fiber's own stack. Reads `this` back out of t_fiber_start_arg
    // (stashed by poll() immediately before the switch that starts us), invokes
    // the entry callable, stores its result/exception (exactly like
    // BlockingState<T> does for spawn_blocking), and switches back to the
    // caller. Never returns: the caller's poll() only ever resumes this stack
    // again via switch_context(), never by falling off the end of this function.
    static void trampoline() {
        // Completes the ASan fiber-switch handshake for this stack's first-ever
        // entry -- see notify_fiber_switch_finished()'s comment. Must come
        // before anything else touches this stack.
        detail::notify_fiber_switch_finished();
        auto* self = static_cast<FiberFuture*>(detail::t_fiber_start_arg);
        try {
            if constexpr (std::is_void_v<T>) self->m_entry();
            else self->m_result = self->m_entry();
        } catch (...) {
            self->m_exception = std::current_exception();
        }
        self->m_finished = true;
        detail::switch_context(&self->m_fiber_ctx, &self->m_caller_ctx);
    }

    std::function<T()> m_entry;
    size_t              m_stack_size;
    detail::FiberContext m_caller_ctx{};
    detail::FiberContext m_fiber_ctx{};
    bool                m_started{false};
    bool                m_finished{false};
    std::exception_ptr  m_exception;
    ResultStorage       m_result{};
};

namespace detail {

/// @brief Hands control back to whoever called poll(), suspending the fiber
/// exactly where it is. Throws std::logic_error if called with no fiber
/// currently active on this thread — fail clean rather than switch into
/// garbage.
///
/// Deliberately internal, not part of the public API: unlike `fiber_await()`,
/// this is a bare context switch with no contract that anything will ever
/// wake the fiber back up — calling it without first registering a waker
/// (as `fiber_await()` does, via the future's own `poll()`) permanently wedges
/// the fiber. `fiber_await()` is the only sanctioned way for a fiber body to
/// suspend; this exists only as its building block (and to let tests drive
/// raw multi-poll suspension directly).
void fiber_yield();

} // namespace detail

/**
 * @brief The fiber-land equivalent of `co_await`: polls `future` until it's
 * ready, yielding back to the caller (via `detail::fiber_yield()`) between
 * polls so other coro tasks keep making progress in the meantime.
 *
 * Must be called from inside a fiber body (i.e. inside the callable passed to
 * `spawn_fiber()`, or something it calls) — throws std::logic_error otherwise.
 */
template<Future F>
typename F::OutputType fiber_await(F future) {
    if (!detail::t_current_fiber_ctx)
        throw std::logic_error("fiber_await() called outside a fiber");
    for (;;) {
        auto r = future.poll(*detail::t_current_fiber_ctx);
        if (!r.isPending()) {
            r.rethrowIfError();
            if constexpr (std::is_void_v<typename F::OutputType>) return;
            else return std::move(r).value();
        }
        detail::fiber_yield(); // hand control back; resumes here once woken + re-polled
    }
}

/**
 * @brief Handle to a running fiber. Returned by `spawn_fiber()`. Satisfies `Future<T>`.
 *
 * Modeled on `BlockingHandle<T>` (spawn_blocking.h), not `JoinHandle<T>`: there is no
 * `.cancel()` method at all, and dropping the handle *always* detaches, never cancels —
 * not merely a default that can be overridden. Once execution has entered the entry
 * callable's non-coroutine call frames, there is no compiler-generated hook to unwind
 * through safely (same restriction `BlockingHandle<T>` documents for spawn_blocking's
 * worker threads), so the capability isn't offered in the first place.
 *
 * **Awaiting:** `co_await handle` suspends the caller until the fiber runs to completion
 * and returns its value (or rethrows its exception).
 *
 * **Dropping:** the destructor detaches unconditionally. The executor's owned map keeps
 * the fiber alive and keeps polling it to completion in the background; the entry
 * callable's return value, if `T` isn't `void`, is simply discarded once nothing is left
 * to observe it.
 *
 * Internally wraps a `JoinHandle<T>` purely as plumbing — a fiber can only make progress
 * by being polled by the executor, which is exactly what `coro::spawn()`/`JoinHandle<T>`
 * already provide. `FiberHandle<T>` never calls `.cancel()` on it and always `.detach()`es
 * on drop, so `JoinHandle<T>`'s cancellation machinery is simply never reached.
 */
template<typename T>
class [[nodiscard]] FiberHandle {
public:
    using OutputType = T;

    explicit FiberHandle(JoinHandle<T> handle) noexcept : m_handle(std::move(handle)) {}

    FiberHandle(const FiberHandle&)            = delete;
    FiberHandle& operator=(const FiberHandle&) = delete;
    FiberHandle(FiberHandle&&) noexcept            = default;
    FiberHandle& operator=(FiberHandle&&) noexcept = default;

    /// @brief Detaches — never cancels. Not configurable, unlike JoinHandle<T>'s
    /// cancelOnDestroy: a fiber has no safe unwind point to cancel through.
    ~FiberHandle() { m_handle.detach(); }

    PollResult<T> poll(detail::Context& ctx) { return m_handle.poll(ctx); }

private:
    JoinHandle<T> m_handle;
};

/**
 * @brief User-facing entry point — spawns `entry` to run on its own stack,
 * cooperatively scheduled alongside every other coro task. Modeled on
 * `spawn_blocking()`'s shape but for bare-metal/single-thread-affinity code
 * that can't run on a worker thread — see doc/design/fiber.md.
 *
 * Returns a `FiberHandle<T>` that may be `co_await`ed for the result, or simply
 * discarded (`[[maybe_unused]] auto h = spawn_fiber(...);`) for pure fire-and-forget —
 * either way, the fiber is never cancellable, and dropping the handle always detaches.
 *
 * `stack_size` has no default: the Pico backend has no safe universal
 * default to fall back to (see "Stack allocation and sizing" in
 * doc/design/fiber.md), so every call site is required to pass one
 * explicitly on both backends.
 */
template<typename T>
[[nodiscard]] FiberHandle<T> spawn_fiber(std::function<T()> entry, size_t stack_size) {
    return FiberHandle<T>(coro::spawn(FiberFuture<T>(std::move(entry), stack_size)));
}

} // namespace coro
