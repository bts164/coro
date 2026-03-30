#pragma once

#include <coro/runtime/executor.h>
#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coro/task/spawn_builder.h>
#include <coro/stream.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace coro {

// Forward declarations — set_current_runtime must be visible inside block_on's body
// because non-dependent names in templates are resolved at the point of definition.
class Runtime;
void set_current_runtime(Runtime* rt);
Runtime& current_runtime();

// Runtime — owns the executor and (eventually) the thread pool and libuv event loop.
// Entry point: construct a Runtime, then call block_on() from main().
class Runtime {
public:
    explicit Runtime(std::size_t num_threads = std::thread::hardware_concurrency());
    ~Runtime();

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Runs future on the calling thread, blocking until it completes.
    // Sets the thread-local runtime for the duration of the call.
    // TODO: drive libuv event loop alongside the executor.
    template<Future F>
    typename F::OutputType block_on(F future) {
        set_current_runtime(this);

        auto state = std::make_shared<detail::TaskState<typename F::OutputType>>();
        m_executor->schedule(
            std::make_unique<detail::Task>(std::move(future), state));

        auto is_done = [&]() -> bool {
            std::lock_guard lock(state->mutex);
            return state->terminated;
        };

        while (!is_done()) {
            if (!m_executor->poll_ready_tasks())
                break;  // queue empty — no I/O support yet, so we're stuck
        }

        set_current_runtime(nullptr);

        if (state->exception)
            std::rethrow_exception(state->exception);
        if constexpr (!std::is_void_v<typename F::OutputType>)
            return std::move(*state->result);
    }

    // Submit a pre-constructed task directly. Used internally by Synchronize.
    void schedule_task(std::unique_ptr<detail::Task> task) {
        m_executor->schedule(std::move(task));
    }

    // Returns a builder for submitting a future as a scheduled task.
    template<Future F>
    [[nodiscard]] SpawnBuilder<F> spawn(F future) {
        return SpawnBuilder<F>(std::move(future), m_executor.get());
    }

    // Returns a builder for submitting a stream as a background task.
    template<Stream S>
    [[nodiscard]] StreamSpawnBuilder<S> spawn(S stream) {
        return StreamSpawnBuilder<S>(std::move(stream), m_executor.get());
    }

private:
    std::unique_ptr<Executor> m_executor;
};


// Sets the thread-local runtime for the calling thread.
// Called internally by Runtime::block_on() and worker thread startup.
void set_current_runtime(Runtime* rt);

// Returns the thread-local runtime. Throws if called outside a runtime context.
Runtime& current_runtime();

// Free spawn — equivalent to current_runtime().spawn(std::move(future)).
template<Future F>
[[nodiscard]] SpawnBuilder<F> spawn(F future) {
    return current_runtime().spawn(std::move(future));
}

// Free spawn for streams — equivalent to current_runtime().spawn(std::move(stream)).
template<Stream S>
[[nodiscard]] StreamSpawnBuilder<S> spawn(S stream) {
    return current_runtime().spawn(std::move(stream));
}

} // namespace coro
