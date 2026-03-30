#pragma once

#include <coro/detail/task.h>
#include <memory>

namespace coro {

// Abstract executor interface — responsible only for scheduling.
// Accepts type-erased tasks and decides when to poll them.
// Does not own threads or the I/O reactor; Runtime owns those.
//
// Concrete implementations: SingleThreadedExecutor (first), work-stealing (second).
class Executor {
public:
    virtual ~Executor();

    // Submit a task for scheduling. The executor takes ownership.
    virtual void schedule(std::unique_ptr<detail::Task> task) = 0;

    // Poll all currently-ready tasks once.
    // Returns true if at least one task was processed; false if the queue was empty.
    virtual bool poll_ready_tasks() = 0;
};

} // namespace coro
