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

// A type-erased pending child entry — one per JoinHandle dropped without co_awaiting.
struct PendingChild {
    std::function<bool()>                              is_done;
    std::function<void(std::shared_ptr<Waker>)>        set_scope_waker;
};

// CoroutineScope — embedded in each Coro<T>/CoroStream<T> to track spawned children
// whose JoinHandles were dropped without being co_await-ed to completion.
//
// The coroutine delays its next suspension or completion (and its own PollDropped) until
// all registered children have signalled done via their scope_waker.
class CoroutineScope {
public:
    // Register a pending child. Called from JoinHandle::~JoinHandle while
    // t_current_coro points to this scope.
    void add_child(std::function<bool()>                       is_done,
                   std::function<void(std::shared_ptr<Waker>)> set_scope_waker) {
        std::lock_guard lock(m_mutex);
        m_pending.push_back({std::move(is_done), std::move(set_scope_waker)});
    }

    // Sweep completed children and return true if any remain.
    bool has_pending() {
        std::lock_guard lock(m_mutex);
        m_pending.erase(
            std::remove_if(m_pending.begin(), m_pending.end(),
                [](const PendingChild& c) { return c.is_done(); }),
            m_pending.end());
        return !m_pending.empty();
    }

    // Register the drain waker on all pending children using a double-check to handle
    // the race where a child completes between has_pending() and this call.
    // Returns true if any children are still pending after the double-check.
    bool set_drain_waker(std::shared_ptr<Waker> waker) {
        std::lock_guard lock(m_mutex);
        m_drain_waker = waker;
        // First pass: remove already-done children
        m_pending.erase(
            std::remove_if(m_pending.begin(), m_pending.end(),
                [](const PendingChild& c) { return c.is_done(); }),
            m_pending.end());
        if (m_pending.empty()) return false;
        // Set scope_waker on remaining children
        for (auto& child : m_pending)
            child.set_scope_waker(waker);
        // Second pass: a child may have completed between the first sweep and set_scope_waker
        m_pending.erase(
            std::remove_if(m_pending.begin(), m_pending.end(),
                [](const PendingChild& c) { return c.is_done(); }),
            m_pending.end());
        return !m_pending.empty();
    }

private:
    std::mutex                m_mutex;
    std::vector<PendingChild> m_pending;
    std::shared_ptr<Waker>    m_drain_waker;
};

// Thread-local pointer to the coroutine currently executing inside poll().
// Set by CurrentCoroGuard for the duration of each poll() call so that
// JoinHandle destructors can identify their scope owner.
inline thread_local CoroutineScope* t_current_coro = nullptr;

// RAII guard — sets t_current_coro for the duration of a poll() call and
// restores the previous value (supporting nested coroutine polls correctly).
struct CurrentCoroGuard {
    CoroutineScope* m_previous;
    explicit CurrentCoroGuard(CoroutineScope* scope) noexcept
        : m_previous(t_current_coro) { t_current_coro = scope; }
    ~CurrentCoroGuard() noexcept { t_current_coro = m_previous; }
    CurrentCoroGuard(const CurrentCoroGuard&)            = delete;
    CurrentCoroGuard& operator=(const CurrentCoroGuard&) = delete;
};

} // namespace coro::detail
