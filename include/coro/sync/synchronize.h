#pragma once

#include <coro/coro.h>
#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coro/runtime/runtime.h>
#include <coro/detail/task.h>
#include <coro/detail/task_state.h>
#include <coro/detail/waker.h>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace coro {

class Synchronize;

namespace detail {

// Type-erased interface for tracking a spawned child task.
struct SyncChildBase {
    virtual ~SyncChildBase()                              = default;
    virtual bool               is_done() const            = 0;
    virtual std::exception_ptr exception() const          = 0;
    virtual void               set_waker(std::shared_ptr<Waker>) = 0;
};

template<typename T>
struct SyncChild final : SyncChildBase {
    std::shared_ptr<TaskState<T>> state;

    explicit SyncChild(std::shared_ptr<TaskState<T>> s) : state(std::move(s)) {}

    bool is_done() const override {
        std::lock_guard lock(state->mutex);
        if constexpr (std::is_void_v<T>)
            return state->done || state->exception != nullptr;
        else
            return state->result.has_value() || state->exception != nullptr;
    }

    std::exception_ptr exception() const override {
        std::lock_guard lock(state->mutex);
        return state->exception;
    }

    void set_waker(std::shared_ptr<Waker> waker) override {
        std::lock_guard lock(state->mutex);
        state->join_waker = std::move(waker);
    }
};

} // namespace detail

// SyncSpawnBuilder — returned by Synchronize::spawn().
// submit() registers the child with the scope and schedules it on the runtime.
// Returns void (not JoinHandle) — the scope owns child lifetime.
template<Future F>
class [[nodiscard]] SyncSpawnBuilder {
public:
    using OutputType = typename F::OutputType;

    SyncSpawnBuilder(F future, Synchronize* sync)
        : m_future(std::move(future)), m_sync(sync) {}

    SyncSpawnBuilder(const SyncSpawnBuilder&)            = delete;
    SyncSpawnBuilder& operator=(const SyncSpawnBuilder&) = delete;
    SyncSpawnBuilder(SyncSpawnBuilder&&)                 = default;

    // Schedules the future on the current runtime and registers it with the
    // enclosing Synchronize scope. Must be called exactly once.
    void submit() &&;

private:
    F            m_future;
    Synchronize* m_sync;
};

// Synchronize — structured concurrency scope.
// Satisfies Future<void>.
//
// All tasks spawned through sync.spawn() inside the callable are guaranteed to
// complete before the co_await returns, even if an exception unwinds the body.
//
// Usage:
//   co_await Synchronize([&](Synchronize& sync) -> Coro<void> {
//       sync.spawn(child_a()).submit();
//       sync.spawn(child_b()).submit();
//   });
class Synchronize {
public:
    using OutputType = void;

    template<typename Fn>
    explicit Synchronize(Fn fn)
        : m_fn(std::move(fn)) {}

    Synchronize(const Synchronize&)            = delete;
    Synchronize& operator=(const Synchronize&) = delete;
    Synchronize(Synchronize&&)                 = default;
    Synchronize& operator=(Synchronize&&)      = default;

    template<Future F>
    [[nodiscard]] SyncSpawnBuilder<F> spawn(F future) {
        return SyncSpawnBuilder<F>(std::move(future), this);
    }

    // Called by SyncSpawnBuilder::submit() to register a child.
    void add_child(std::unique_ptr<detail::SyncChildBase> child) {
        m_children.push_back(std::move(child));
    }

    PollResult<void> poll(detail::Context& ctx) {
        // First poll: create the body coroutine by invoking the user's lambda.
        if (!m_body) {
            m_body.emplace(m_fn(*this));
        }

        // Drive body until it completes or suspends.
        if (!m_body_done) {
            auto result = m_body->poll(ctx);
            if (result.isError()) {
                if (!m_first_exception)
                    m_first_exception = result.error();
                m_body_done = true;
            } else if (result.isPending()) {
                return PollPending;
            } else {
                m_body_done = true;
            }
        }

        // Body is done — wait for all children.
        bool any_pending = false;
        for (auto& child : m_children) {
            if (child->is_done()) {
                // Capture first child exception (if any).
                if (!m_first_exception) {
                    auto ex = child->exception();
                    if (ex) m_first_exception = ex;
                }
            } else {
                // Register our waker so the child can wake us when it finishes.
                child->set_waker(ctx.getWaker()->clone());
                any_pending = true;
            }
        }

        if (any_pending) return PollPending;

        if (m_first_exception) return PollError(m_first_exception);
        return PollReady;
    }

private:
    std::function<Coro<void>(Synchronize&)>        m_fn;
    std::optional<Coro<void>>                      m_body;
    bool                                           m_body_done = false;
    std::vector<std::unique_ptr<detail::SyncChildBase>> m_children;
    std::exception_ptr                             m_first_exception;
};

// Defined after Synchronize is complete so add_child() is available.
template<Future F>
void SyncSpawnBuilder<F>::submit() && {
    auto state = std::make_shared<detail::TaskState<OutputType>>();
    m_sync->add_child(std::make_unique<detail::SyncChild<OutputType>>(state));
    current_runtime().schedule_task(
        std::make_unique<detail::Task>(std::move(m_future), state));
}

} // namespace coro
