#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#ifndef CORO_PICO
#include <coro/detail/blocking_cancel.h>
#include <coro/detail/blocking_waker.h>
#include <type_traits>
#endif

namespace coro {

/**
 * @brief C++20 concept modelling an asynchronous value.
 *
 * Mirrors Rust's `Future` trait. A type `F` satisfies `Future` if it exposes:
 * - `F::OutputType` — the type produced when the future completes.
 * - `PollResult<F::OutputType> F::poll(Context&)` — advances the future toward completion.
 *
 * `poll()` returns one of four states:
 * - `PollPending`  — not yet ready; the waker in `ctx` has been registered for a future wake-up.
 * - `PollReady`    — completed successfully; the result is embedded in the return value.
 * - `PollError`    — faulted; an exception is embedded in the return value.
 * - `PollDropped`  — cancelled and fully drained; propagates up the call chain.
 *
 * `poll()` **must not** be called again after it returns `PollReady`, `PollError`, or `PollDropped`.
 *
 * @tparam F The candidate type to check.
 */
template<typename F>
concept Future = requires(F& f, detail::Context& ctx) {
    typename F::OutputType;
    { F(std::move(f)) };
    { f.poll(ctx) } -> std::same_as<PollResult<typename F::OutputType>>;
};

/**
 * @brief Concept satisfied by futures that own an internal execution tree and must be
 * drained before they can be safely destroyed.
 *
 * A type satisfying `Cancellable` declares: "I cannot simply be dropped mid-execution.
 * Call `cancel()` on me, then poll me until I return `PollDropped`."  The canonical
 * implementations are @ref Coro and @ref CoroStream, which may be awaiting nested
 * children that hold references to the caller's frame locals.
 *
 * A future that does **not** satisfy `Cancellable` is a leaf future: it does not await
 * children, holds no references to the caller's locals, and can be destroyed at any time
 * (its destructor handles immediate cleanup such as removing a stored waker). Examples:
 * channel send/receive futures, `EventFuture`, timer futures.
 *
 * The cancellation protocol in `Coro<T>::poll()` uses this distinction: Cancellable
 * awaited futures are drained before `handle.destroy()` is called; non-Cancellable ones
 * are dropped as part of the LIFO teardown in `handle.destroy()`.
 *
 * @tparam F A type satisfying @ref Future.
 */
template<typename F>
concept Cancellable = Future<F> && requires(F& f) { f.cancel(); };


/**
 * @brief Non-owning wrapper that makes a borrowed future usable where a future is expected.
 *
 * Returned by `coro::ref(f)`. Holds a raw pointer to `f` and delegates `poll()` to the
 * underlying future without taking ownership of it.
 *
 * The primary use-case is passing a future to `select()` (or `join()`, `timeout()`, etc.)
 * without consuming it — if the branch loses, the underlying future keeps running and can
 * be used again in the next round.
 *
 * **Lifetime:** `FutureRef<F>` holds a raw pointer to `f`. `f` must outlive the
 * `FutureRef`. The intended usage pattern — `co_await select(coro::ref(f), other)` — is
 * naturally safe because `f` is a named local in the same scope and the `FutureRef` is a
 * temporary destroyed when the `co_await` returns.
 *
 * **Result consumption:** if the `FutureRef` branch wins and delivers a result, the result
 * is moved out of `f`. Do not await `f` again after that — it is logically spent.
 *
 * **Cancellation:** `FutureRef` is never `Cancellable`, regardless of whether `F` is.
 * When a `select()` branch backed by a `FutureRef` loses, `select()` simply drops the
 * wrapper — the underlying future keeps running untouched. Any waker the losing poll
 * registered remains live and may cause a spurious wake-up on the next select round;
 * this is harmless because `poll()` is required to handle spurious calls gracefully.
 *
 * `FutureRef` is non-copyable and movable.
 *
 * @tparam F A type satisfying @ref Future.
 */
template<Future F>
class FutureRef {
public:
    using OutputType = typename F::OutputType;

    explicit FutureRef(F& f) noexcept : m_future(&f) {}

    FutureRef(const FutureRef&)            = delete;
    FutureRef& operator=(const FutureRef&) = delete;
    FutureRef(FutureRef&&) noexcept            = default;
    FutureRef& operator=(FutureRef&&) noexcept = default;

    PollResult<OutputType> poll(detail::Context& ctx) {
        return m_future->poll(ctx);
    }

    // cancel() is intentionally absent. FutureRef is a non-owning view; cancelling it
    // would cancel the underlying future, which is the opposite of what ref() is for.
    // select() will simply drop the FutureRef when a branch loses, leaving the underlying
    // future running. Any waker the losing poll registered remains live and may fire
    // spuriously — this is accepted as a minor inefficiency; spurious polls are part of
    // the poll() contract.

private:
    F* m_future;
};

/**
 * @brief Wraps `f` in a `FutureRef<F>` — a non-owning future that delegates to `f`.
 *
 * Only accepts lvalues. This prevents accidentally wrapping a temporary (which would
 * immediately dangle). The typical usage:
 *
 * @code
 * JoinHandle<int> task = spawn(work());
 * // task keeps running if the other branch wins:
 * auto sel = co_await select(coro::ref(task), signal);
 * @endcode
 *
 * See @ref FutureRef for the full contract.
 */
template<Future F>
[[nodiscard]] FutureRef<F> ref(F& f) noexcept {
    return FutureRef<F>(f);
}

/**
 * @brief Future that never completes. `poll()` always returns `PollPending`.
 *
 * Returned by `coro::never<T>()`. Useful as a stable placeholder branch — e.g. in
 * `select()` — for a case that should simply never win. See `WhenFuture`
 * (`coro/sync/when.h`) for the common case of a branch that's only sometimes present:
 * it wraps this same "always Pending" idea behind a runtime condition, without the
 * caller needing to name `T` explicitly.
 *
 * @tparam T The (unused) output type; must match the other branches' expectations.
 */
template<typename T>
class NeverFuture {
public:
    using OutputType = T;

    PollResult<T> poll(detail::Context&) { return PollPending; }
};

/// @brief Returns a future that never completes. See @ref NeverFuture.
template<typename T>
[[nodiscard]] NeverFuture<T> never() noexcept {
    return NeverFuture<T>{};
}

#ifndef CORO_PICO

/**
 * @brief Polls @p future to completion on the calling thread, blocking the OS thread
 * between polls. Returns the future's value, or rethrows its exception.
 *
 * Unlike `Runtime::block_on()`, this does not create an executor or a reactor — it
 * loop-polls `future` directly, reusing whatever `current_runtime()` context is already
 * active on the calling thread (ambient on a `spawn_blocking` thread; see
 * `coro::spawn_blocking()`). A future that touches the runtime (a timer, a socket, a
 * child task) requires that context to be active — `current_runtime()` throws
 * `std::runtime_error` otherwise, the same as it would from any other thread with no
 * active runtime.
 *
 * Never call it from a coroutine: it blocks the executor thread.
 *
 * **Cancellation.** Inside a `spawn_blocking` callable this is a cancellation point. If
 * the blocking task has been asked to cancel (and no `BlockingCancelShield` is alive),
 * it throws @ref BlockingCancelled, whether the request was already pending on entry or
 * arrives while waiting. Before it throws, `future` is shut down the way a cancelled
 * task shuts down the future it runs: a @ref Cancellable future is cancelled and polled
 * until it has drained, so that whatever it owns (a coroutine frame, its children) is
 * gone before the caller's stack unwinds; a leaf future is destroyed. Whatever the
 * drained future produced, even a value, is discarded. The drain itself cannot be
 * cancelled.
 *
 * **A dropped future.** If `future` reports `PollDropped`, there is no value to return:
 * this throws @ref BlockingCancelled, for every output type, on every thread, shielded
 * or not. That is the synchronous counterpart of a coroutine being dropped at a
 * `co_await`.
 *
 * See doc/design/blocking_wait.md for the full design and rationale.
 *
 * @tparam F A type satisfying @ref Future.
 */
template<Future F>
typename F::OutputType blocking_wait(F future) {
    detail::BlockingTaskBase* const task = detail::current_blocking_task();

    if (task == nullptr) {
        // Not a blocking pool thread: park on a private condvar-backed waker.
        auto waker = detail::make_rc<detail::BlockingWaker>();
        detail::Context ctx(waker->clone());
        for (;;) {
            auto r = future.poll(ctx);
            if (r.isDropped()) throw BlockingCancelled{};
            if (!r.isPending()) {
                r.rethrowIfError();
                if constexpr (std::is_void_v<typename F::OutputType>) return;
                else return std::move(r).value();
            }
            waker->wait_for_wake();
        }
    }

    // On a blocking pool thread. The blocking task is the waker, so that cancel_task()
    // (which wakes the task) reaches this thread while it is parked here. Wakers left
    // behind in futures an earlier blocking_wait() polled can also wake it; park()
    // returning only ever means "poll again", so those are harmless.
    detail::Context ctx(detail::blocking_task_waker(*task));

    // Already cancelled on entry: don't start waiting on anything, go straight to
    // shutting `future` down.
    if (!detail::blocking_cancel_pending(*task)) {
        for (;;) {
            auto r = future.poll(ctx);
            if (r.isDropped()) throw BlockingCancelled{};
            if (!r.isPending()) {
                r.rethrowIfError();
                if constexpr (std::is_void_v<typename F::OutputType>) return;
                else return std::move(r).value();
            }
            detail::blocking_task_park(*task);
            // RACE: the cancelled flag is set before the task is woken, so a park()
            // that returned because of a cancel always sees it here. A cancel that
            // lands after this check is caught after the next park(), which returns at
            // once because the cancel's wake left the task RunningAndNotified.
            if (detail::blocking_cancel_pending(*task)) break;
        }
    }

    // Cancelled. This is what TaskImpl::poll() does with the future of a cancelled
    // task, and for the same reason: a Cancellable future may own a coroutine frame
    // and children that hold references into things that are about to unwind, so it
    // has to be drained, not just dropped. Cancel it once and poll until it is no
    // longer pending, even if it was never polled before (a coroutine that has not
    // started still owns its arguments, and a JoinHandle its running task). Its
    // outcome is discarded, even a value. A leaf future is simply destroyed.
    if constexpr (Cancellable<F>) {
        future.cancel();
        for (;;) {
            auto r = future.poll(ctx);
            if (!r.isPending()) break;
            detail::blocking_task_park(*task);
        }
    }
    // `future` is destroyed as the exception leaves this function.
    throw BlockingCancelled{};
}

#endif // CORO_PICO

} // namespace coro
