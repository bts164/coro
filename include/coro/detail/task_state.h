#pragma once

// Internal — not part of the public API.
// Shared ref-counted state between an executor-held Task and its JoinHandle.

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

#include <coro/detail/waker.h>

namespace coro::detail {

// TaskState<T> — shared state for tasks that produce a value of type T.
template<typename T>
struct TaskState {
    mutable std::mutex     mutex;
    std::atomic<bool>      cancelled{false};
    std::shared_ptr<Waker> join_waker;   // set by JoinHandle::poll; called when task completes
    std::shared_ptr<Waker> scope_waker;  // fired alongside join_waker when task completes
    std::optional<T>       result;
    std::exception_ptr     exception;
    bool                   terminated{false};

    bool is_complete() const {
        std::lock_guard lock(mutex);
        return terminated;
    }

    // Called by Task when cancelled (no result set, just signals done)
    void mark_done() {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }

    // Called by the Task when it completes successfully.
    void setResult(T value) {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            result = std::move(value);
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }

    // Called by the Task when it completes with an unhandled exception.
    void setException(std::exception_ptr e) {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            exception = std::move(e);
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }
};

// TaskState<void> — specialization for tasks with no return value.
template<>
struct TaskState<void> {
    mutable std::mutex     mutex;
    std::atomic<bool>      cancelled{false};
    std::shared_ptr<Waker> join_waker;
    std::shared_ptr<Waker> scope_waker;  // fired alongside join_waker when task completes
    bool                   done{false};
    std::exception_ptr     exception;
    bool                   terminated{false};

    bool is_complete() const {
        std::lock_guard lock(mutex);
        return terminated;
    }

    // Called by Task when cancelled (no result set, just signals done)
    void mark_done() {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }

    void setDone() {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            done = true;
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }

    void setException(std::exception_ptr e) {
        std::shared_ptr<Waker> join_wk, scope_wk;
        {
            std::lock_guard lock(mutex);
            exception = std::move(e);
            terminated = true;
            join_wk = std::move(join_waker);
            scope_wk = std::move(scope_waker);
        }
        if (join_wk)  join_wk->wake();
        if (scope_wk) scope_wk->wake();
    }
};

} // namespace coro::detail
