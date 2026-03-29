#pragma once

// Internal — not part of the public API.
// Shared ref-counted state between an executor-held Task and its JoinHandle.

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

#include <coro/waker.h>

namespace coro::detail {

// TaskState<T> — shared state for tasks that produce a value of type T.
template<typename T>
struct TaskState {
    mutable std::mutex     mutex;
    std::atomic<bool>      cancelled{false};
    std::shared_ptr<Waker> join_waker;   // set by JoinHandle::poll; called when task completes
    std::optional<T>       result;
    std::exception_ptr     exception;

    // Called by the Task when it completes successfully.
    void setResult(T value) {
        std::shared_ptr<Waker> to_wake;
        {
            std::lock_guard lock(mutex);
            result = std::move(value);
            to_wake = join_waker;
        }
        if (to_wake) to_wake->wake();
    }

    // Called by the Task when it completes with an unhandled exception.
    void setException(std::exception_ptr e) {
        std::shared_ptr<Waker> to_wake;
        {
            std::lock_guard lock(mutex);
            exception = std::move(e);
            to_wake = join_waker;
        }
        if (to_wake) to_wake->wake();
    }
};

// TaskState<void> — specialization for tasks with no return value.
template<>
struct TaskState<void> {
    mutable std::mutex     mutex;
    std::atomic<bool>      cancelled{false};
    std::shared_ptr<Waker> join_waker;
    bool                   done{false};
    std::exception_ptr     exception;

    void setDone() {
        std::shared_ptr<Waker> to_wake;
        {
            std::lock_guard lock(mutex);
            done = true;
            to_wake = join_waker;
        }
        if (to_wake) to_wake->wake();
    }

    void setException(std::exception_ptr e) {
        std::shared_ptr<Waker> to_wake;
        {
            std::lock_guard lock(mutex);
            exception = std::move(e);
            to_wake = join_waker;
        }
        if (to_wake) to_wake->wake();
    }
};

} // namespace coro::detail
