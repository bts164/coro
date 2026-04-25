#pragma once

// Internal — not part of the public API.
// Per-coroutine pending-child tracking for implicit structured concurrency.
// See doc/coroutine_scope.md for the full design.

#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace coro::detail {

/**
 * @brief Type-erased record of a single pending child task.
 *
 * Registered by `JoinHandle::~JoinHandle()` when a handle is dropped inside a
 * coroutine's `poll()` call. Holds two callbacks over the shared `TaskState`:
 * - `is_done` — polled by `CoroutineScope` to sweep completed children.
 * - `set_scope_waker` — called to install the drain waker so the child can
 *   wake the parent when it finishes.
 */
struct PendingChild {
    std::function<bool()>                              is_done;
    std::function<void(std::shared_ptr<Waker>)>        set_scope_waker;
};

/**
 * @brief Per-coroutine scope that tracks spawned children whose `JoinHandle`s were dropped.
 *
 * Every `Coro<T>` and `CoroStream<T>` owns a `CoroutineScope` as a direct value member.
 * The scope provides the *implicit structured concurrency* guarantee: a coroutine's frame
 * is not freed until all children it spawned and then dropped have themselves finished.
 *
 * `CoroutineScope` contains a `std::mutex` and is therefore not copyable. It is movable
 * via a custom move constructor that moves the pending-child list and default-constructs
 * a fresh mutex at the destination. Moves only occur before the first `poll()` call
 * (when `Coro<T>` is returned from a coroutine function and moved into a `Task`), at
 * which point `m_pending` is always empty and the mutex is in its default unlocked state.
 *
 * ### Registration
 * When a `JoinHandle` destructor fires while `t_current_coro` is non-null, it calls
 * `add_child()` to register the child's `TaskState` callbacks with the enclosing scope.
 *
 * ### Drain
 * After the coroutine frame is destroyed, `Coro::poll()` calls `set_drain_waker()`. This
 * installs a waker on every pending child so they can wake the parent when they finish.
 * Once all children are done, `poll()` delivers `PollDropped`.
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
     * @brief Registers a pending child. Called from `JoinHandle::~JoinHandle()` while
     * `t_current_coro` points to this scope.
     */
    void add_child(std::function<bool()>                       is_done,
                   std::function<void(std::shared_ptr<Waker>)> set_scope_waker) {
        std::lock_guard lock(m_mutex);
        m_pending.push_back({std::move(is_done), std::move(set_scope_waker)});
    }

    /**
     * @brief Sweeps completed children and returns `true` if any remain pending.
     */
    bool has_pending() {
        std::lock_guard lock(m_mutex);
        m_pending.erase(
            std::remove_if(m_pending.begin(), m_pending.end(),
                [](const PendingChild& c) { return c.is_done(); }),
            m_pending.end());
        return !m_pending.empty();
    }

    /**
     * @brief Installs `waker` on all pending children and returns `true` if any remain.
     *
     * Uses a double-sweep to close the race between `has_pending()` and waker installation:
     * 1. Remove already-done children.
     * 2. Install the waker on remaining children.
     * 3. Sweep again — a child may have completed between steps 1 and 2.
     *
     * @return `true` if at least one child is still pending after the double-sweep.
     *
     * MEMORY CYCLE WARNING — do NOT store `waker` as a member of this class.
     *
     * `waker` is a TaskWaker that holds a `shared_ptr<Task>` for the parent coroutine.
     * That Task owns (via unique_ptr) the Coro<T>, which owns this CoroutineScope as a
     * value member.  Storing `waker` here would create the cycle:
     *
     *   Task → Coro<T> → CoroutineScope → TaskWaker → Task
     *
     * When the Task completes and the executor drops its last local shared_ptr<Task>,
     * the stored TaskWaker would be the only remaining owner, so the Task's ref count
     * never reaches zero and the entire chain leaks permanently.
     *
     * Each pending child already holds its own clone of the waker via
     * TaskState::scope_waker, which is sufficient to re-enqueue the parent when the
     * child finishes.  No member storage of the waker is needed.
     */
    bool set_drain_waker(std::shared_ptr<Waker> waker) {
        std::lock_guard lock(m_mutex);
        m_pending.erase(
            std::remove_if(m_pending.begin(), m_pending.end(),
                [](const PendingChild& c) { return c.is_done(); }),
            m_pending.end());
        if (m_pending.empty()) return false;
        for (auto& child : m_pending)
            child.set_scope_waker(waker);
        m_pending.erase(
            std::remove_if(m_pending.begin(), m_pending.end(),
                [](const PendingChild& c) { return c.is_done(); }),
            m_pending.end());
        return !m_pending.empty();
    }

private:
    std::mutex                m_mutex;
    std::vector<PendingChild> m_pending;
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
