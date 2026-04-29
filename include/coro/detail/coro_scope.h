#pragma once

// Internal — not part of the public API.
// Per-coroutine pending-child tracking for implicit structured concurrency.
// See doc/coroutine_scope.md for the full design.

#include <coro/detail/task.h>
#include <coro/detail/waker.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace coro::detail {

/**
 * @brief Per-coroutine scope that tracks spawned children whose `JoinHandle`s were dropped.
 *
 * Every `Coro<T>` and `CoroStream<T>` owns a `CoroutineScope` as a direct value member.
 * The scope provides the *implicit structured concurrency* guarantee: a coroutine's frame
 * is not freed until all children it spawned and then dropped have themselves finished.
 *
 * The scope owns each pending child via `OwnedTask` — the move-only wrapper that is the
 * sole persistent strong reference to the task. Wakers (used to notify the parent when a
 * child completes) are stored as `weak_ptr<Waker>` on the child's `TaskState`, not here,
 * so no reference cycle is created. See doc/task_ownership.md and doc/shared_ptr_cycles.md.
 *
 * `CoroutineScope` contains a `std::mutex` and is therefore not copyable. It is movable
 * via a custom move constructor that moves the pending-child list and default-constructs
 * a fresh mutex at the destination. Moves only occur before the first `poll()` call
 * (when `Coro<T>` is returned from a coroutine function and moved into a `Task`), at
 * which point `m_pending` is always empty and the mutex is in its default unlocked state.
 *
 * ### Registration
 * When a `JoinHandle` destructor fires while `t_current_coro` is non-null, it calls
 * `add_child()` to transfer ownership of the child's `OwnedTask` to the enclosing scope.
 *
 * ### Drain
 * After the coroutine frame is destroyed, `Coro::poll()` calls `set_drain_waker()`. This
 * installs a weak scope waker on every pending child so they can wake the parent when they
 * finish. Once all children are done, `poll()` delivers `PollDropped`.
 *
 * ### Thread safety
 * All methods are protected by an internal mutex — safe for the multi-threaded executor.
 */
class CoroutineScope {
public:
    CoroutineScope() = default;

    // Move constructor: moves pending children, default-constructs a fresh mutex.
    // Safe because moves only happen before first poll() when m_pending is always empty.
    CoroutineScope(CoroutineScope&& other) noexcept
        : m_pending(std::move(other.m_pending)) {}

    CoroutineScope& operator=(CoroutineScope&& other) noexcept {
        if (this != &other)
            m_pending = std::move(other.m_pending);
        return *this;
    }

    CoroutineScope(const CoroutineScope&)            = delete;
    CoroutineScope& operator=(const CoroutineScope&) = delete;

    /**
     * @brief Takes ownership of a pending child task. Called from `JoinHandle::~JoinHandle()`
     * while `t_current_coro` points to this scope.
     *
     * `OwnedTask` is the sole persistent strong reference to the task; transferring it here
     * means the scope now controls the task's lifetime until it completes.
     */
    void add_child(OwnedTask task) {
        std::lock_guard lock(m_mutex);
        m_pending.push_back(std::move(task));
    }

    /**
     * @brief Sweeps completed children (destroying their OwnedTask) and returns `true` if any remain.
     */
    bool has_pending() {
        std::lock_guard lock(m_mutex);
        m_pending.erase(
            std::remove_if(m_pending.begin(), m_pending.end(),
                [](const OwnedTask& t) { return t.is_complete(); }),
            m_pending.end());
        return !m_pending.empty();
    }

    /**
     * @brief Installs a weak scope waker on all pending children and returns `true` if any remain.
     *
     * Uses a double-sweep to close the race between child completion and waker installation:
     * 1. Remove already-done children (dropping their OwnedTask — freeing the task allocation).
     * 2. Install the weak waker on remaining children via OwnedTask::set_scope_waker().
     * 3. Sweep again — a child may have completed between steps 1 and 2.
     *
     * The waker is stored as `weak_ptr<Waker>` on the child's TaskState::scope_waker.
     * When the child completes it calls `scope_waker.lock()->wake()` to notify the parent.
     * This does not create a reference cycle — see doc/shared_ptr_cycles.md, Cycle 3.
     *
     * @return `true` if at least one child is still pending after the double-sweep.
     */
    bool set_drain_waker(std::weak_ptr<Waker> waker) {
        std::lock_guard lock(m_mutex);
        m_pending.erase(
            std::remove_if(m_pending.begin(), m_pending.end(),
                [](const OwnedTask& t) { return t.is_complete(); }),
            m_pending.end());
        if (m_pending.empty()) return false;
        for (auto& task : m_pending)
            task.set_scope_waker(waker);
        m_pending.erase(
            std::remove_if(m_pending.begin(), m_pending.end(),
                [](const OwnedTask& t) { return t.is_complete(); }),
            m_pending.end());
        return !m_pending.empty();
    }

private:
    std::mutex             m_mutex;
    std::vector<OwnedTask> m_pending;
};

/**
 * @brief Thread-local pointer to the `CoroutineScope` of the coroutine currently executing.
 *
 * Set by `CurrentCoroGuard` for the duration of each `poll()` call. `JoinHandle`
 * destructors read this to identify which scope to register with. Null outside of
 * a coroutine `poll()` context.
 */
inline thread_local CoroutineScope* t_current_coro = nullptr;

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
