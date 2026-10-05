#pragma once

// Internal — not part of the public API.
// Per-coroutine pending-child tracking for implicit structured concurrency.
// See doc/coroutine_scope.md for the full design.

#include <coro/detail/task.h>
#include <coro/detail/waker.h>
#include <coro/detail/rc.h>
#include <algorithm>
#include <memory>
#include <vector>

namespace coro::detail {

/**
 * @brief Per-coroutine scope that tracks spawned children whose `JoinHandle`s were dropped.
 *
 * Every `Coro<T>` and `CoroStream<T>` owns a `CoroutineScope` as a direct value member.
 * The scope provides the *implicit structured concurrency* guarantee: a coroutine's frame
 * is not freed until all children it spawned and then dropped have themselves finished.
 *
 * The scope tracks each pending child via `weak_ptr<TaskBase>`. Lifetime is owned by the
 * executor's `m_owned_tasks` map from spawn to terminal state — no ownership transfer is
 * needed when a JoinHandle is dropped. Wakers (used to notify the parent when a child
 * completes) are stored as `weak_ptr<Waker>` on the child's `TaskState`, not here,
 * so no reference cycle is created. See doc/task_ownership.md.
 *
 * `CoroutineScope` is move-only. Moves only occur before the first `poll()` call (when
 * `Coro<T>` is returned from a coroutine function and moved into a `Task`), at which
 * point `m_pending` is always empty.
 *
 * ### Registration
 * When a `JoinHandle` destructor fires while `t_current_coro` is non-null, it calls
 * `add_child()` to record a `weak_ptr<TaskBase>` in the scope's pending list.
 *
 * ### Drain
 * After the coroutine frame is destroyed, `Coro::poll()` calls `set_drain_waker()`. This
 * installs a weak scope waker on every pending child so they can wake the parent when they
 * finish. Once all children are done, `poll()` delivers `PollDropped`.
 *
 * ### Thread safety
 * The scope has no lock. `m_pending` is only ever touched by the thread that is currently
 * polling or destroying the owning coroutine, and the executor never runs one task on two
 * threads at once:
 * - `add_child()` is reached only through `t_current_coro`, which points at this scope
 *   only inside a `CurrentCoroGuard` that spans a synchronous `resume()` or `destroy()`
 *   of the owning coroutine on the current thread.
 * - `empty()`, `has_pending()` and `set_drain_waker()` are called only from the owning
 *   coroutine's `poll()`.
 * - A child that finishes on another thread never touches its parent's scope. It fires
 *   the waker stored on its own `TaskState`, under its own mutex.
 * A task may be polled on a different worker each time; the executor's hand-off between
 * workers orders those accesses, as it does for the coroutine frame itself.
 *
 * Potential race: this relies on `t_current_coro` being read on the thread that set it.
 * Fiber code that drops a `JoinHandle` after the fiber has migrated to another worker
 * must re-read the thread-local; if the compiler reused a thread-local address computed
 * before the stack switch, `add_child()` would run against a scope another thread is
 * polling, unsynchronized.
 */
class CoroutineScope {
public:
    CoroutineScope() = default;

    CoroutineScope(CoroutineScope&&) noexcept            = default;
    CoroutineScope& operator=(CoroutineScope&&) noexcept = default;

    CoroutineScope(const CoroutineScope&)            = delete;
    CoroutineScope& operator=(const CoroutineScope&) = delete;

    /**
     * @brief Records a weak reference to a pending child task. Called from
     * `JoinHandle::~JoinHandle()` while `t_current_coro` points to this scope.
     *
     * The executor's owned map keeps the task alive — no ownership transfer is needed.
     */
    void add_child(Weak<TaskBase> task) {
        m_pending.push_back(std::move(task));
    }

    /**
     * @brief Returns `true` if no child is registered, without sweeping.
     *
     * The common case: most coroutines never drop a `JoinHandle`. `poll()` checks this
     * first so that it builds a weak waker only when there is a child to hand it to.
     */
    bool empty() const noexcept { return m_pending.empty(); }

    /**
     * @brief Sweeps completed or expired children and returns `true` if any remain.
     */
    bool has_pending() {
        if (m_pending.empty()) return false;
        m_pending.erase(
            std::remove_if(m_pending.begin(), m_pending.end(),
                [](const Weak<TaskBase>& wp) {
                    auto t = wp.lock();
                    return !t || t->is_complete();
                }),
            m_pending.end());
        return !m_pending.empty();
    }

    /**
     * @brief Installs a weak scope waker on all pending children and returns `true` if any remain.
     *
     * Uses a double-sweep to close the race between child completion and waker installation:
     * 1. Remove already-done or expired children.
     * 2. Install the weak waker on remaining children via TaskBase::set_waker().
     * 3. Sweep again — a child may have completed between steps 1 and 2.
     *
     * The waker is stored as `weak_ptr<Waker>` on the child's TaskState::waker.
     * When the child completes it calls `waker.lock()->wake()` to notify the parent.
     * This does not create a reference cycle — see doc/shared_ptr_cycles.md, Cycle 3.
     *
     * @return `true` if at least one child is still pending after the double-sweep.
     */
    bool set_drain_waker(Weak<Waker> waker) {
        if (m_pending.empty()) return false;
        auto is_done = [](const Weak<TaskBase>& wp) {
            auto t = wp.lock();
            return !t || t->is_complete();
        };
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), is_done),
                        m_pending.end());
        if (m_pending.empty()) return false;
        for (auto& wp : m_pending)
            if (auto t = wp.lock()) t->set_waker(waker);
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), is_done),
                        m_pending.end());
        return !m_pending.empty();
    }

private:
    std::vector<Weak<TaskBase>> m_pending;
};

/**
 * @brief Thread-local pointer to the `CoroutineScope` of the coroutine currently executing.
 *
 * Set by `CurrentCoroGuard` for the duration of each `poll()` call. `JoinHandle`
 * destructors read this to identify which scope to register with. Null outside of
 * a coroutine `poll()` context.
 */
#ifdef CORO_PICO
// Cortex-M0+ has no hardware TLS — thread_local requires __aeabi_read_tp which
// is unavailable in bare-metal newlib. A plain global is equivalent on the
// single-threaded CurrentThreadExecutor.
inline CoroutineScope* t_current_coro = nullptr;
#else
inline thread_local CoroutineScope* t_current_coro = nullptr;
#endif

/**
 * @brief RAII guard that sets `t_current_coro` for the duration of a `poll()` call.
 *
 * Correctly handles nested coroutine polls by restoring the previous value on destruction,
 * so inner coroutines point at their own scope while outer ones point at theirs.
 */
struct CurrentCoroGuard {
    CoroutineScope* m_previous; ///< The scope active before this guard was constructed.

    explicit CurrentCoroGuard(CoroutineScope* scope) noexcept
        : m_previous(t_current_coro) { t_current_coro = scope; }

    ~CurrentCoroGuard() noexcept { t_current_coro = m_previous; }

    CurrentCoroGuard(const CurrentCoroGuard&)            = delete;
    CurrentCoroGuard& operator=(const CurrentCoroGuard&) = delete;
};

} // namespace coro::detail
