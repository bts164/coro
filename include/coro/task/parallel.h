#pragma once

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/wake_batch.h>
#include <coro/future.h>
#include <coro/sync/team.h>
#include <coro/task/join_set.h>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace coro {

namespace detail {

/// What `fn` must be for parallel(): callable as `fn(member, args&...)`, returning a
/// Future<void>. Each part receives its TeamMember by value.
template<typename F, typename... Args>
concept ParallelFn =
    std::invocable<F&, TeamMember, Args&...> &&
    Future<std::invoke_result_t<F&, TeamMember, Args&...>> &&
    std::is_void_v<typename std::invoke_result_t<F&, TeamMember, Args&...>::OutputType>;

/// A part fails with these once another part has thrown or been cancelled: that
/// part's exception is the one parallel() reports. JoinCancelled is a part the first
/// exception cancelled, which may finish before the part that threw.
inline bool parallel_error_ranks_last(const std::exception_ptr& error) noexcept {
    try {
        std::rethrow_exception(error);
    } catch (const TeamBroken&) {
        return true;
    } catch (const TeamRoundBroken&) {
        return true;
    } catch (const JoinCancelled&) {
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace detail

/**
 * @brief `Future<void>` returned by `parallel()`. Cancellable.
 *
 * Spawns the parts at its first poll, then waits for them. Holds `fn`, the args and
 * the parts' `JoinSet`, so that all of them outlive every part: on the
 * first exception the remaining parts are cancelled and the future stays pending
 * until all have finished; `cancel()` does the same and then returns `PollDropped`.
 *
 * Movable only before its first poll. See doc/design/parallel.md.
 */
template<typename F, typename... Args>
    requires detail::ParallelFn<F, Args...>
class ParallelFuture {
public:
    using OutputType = void;

    ParallelFuture(std::size_t k, F fn, Args... args)
        : m_k(k), m_fn(std::move(fn)), m_args(std::move(args)...)
    {
        assert(k >= 1 && "parallel() needs k >= 1");
    }

    /// Only before the first poll: the parts point at the args.
    ParallelFuture(ParallelFuture&& other)
        : m_k(other.m_k)
        , m_fn(std::move(other.m_fn))
        , m_args(std::move(other.m_args))
        , m_cancel_requested(other.m_cancel_requested)
    {
        assert(!other.m_drain && "ParallelFuture moved after its first poll");
    }

    ParallelFuture(const ParallelFuture&)            = delete;
    ParallelFuture& operator=(const ParallelFuture&) = delete;
    ParallelFuture& operator=(ParallelFuture&&)      = delete;

    /// A caller that follows the Cancellable protocol never destroys this future
    /// with parts running. If one does, the JoinSet destructor cancels them, but
    /// they may still be running against the args destroyed here.
    ~ParallelFuture() {
        assert((!m_drain || m_finished) && "ParallelFuture destroyed with parts running");
    }

    /// @brief Cancels every part. Polls then return pending until all have finished,
    /// then `PollDropped`.
    void cancel() {
        m_cancel_requested = true;
        if (m_drain) m_drain->cancel();
    }

    PollResult<void> poll(detail::Context& ctx) {
        if (!m_drain) {
            // Cancelled before the first poll: nothing was spawned.
            if (m_cancel_requested) {
                m_finished = true;
                return PollDropped;
            }
            start();
        }

        PollResult<void> result = m_drain->poll(ctx);
        if (result.isPending()) return result;
        m_finished = true;
        // An exception from starting a part comes first: it is why the rest were
        // cancelled.
        if (m_spawn_error && !result.isDropped()) return PollError(m_spawn_error);
        return result;
    }

private:
    // Spawns the k parts and creates the drain. Leaves m_drain set even if a spawn
    // throws, so that the parts already started are waited for.
    void start() {
        m_drain.emplace(m_set.drain());
        m_drain->set_ranks_last(&detail::parallel_error_ranks_last);
        // One batch for all k spawns: on WorkStealingExecutor they are pushed under one
        // lock and min(k, idle) workers are woken at once. Closed before poll()
        // returns, as a batch must be.
        detail::WakeBatch batch;
        try {
            // Handles not passed to a part when a spawn throws are destroyed here,
            // which breaks the team for the parts already running.
            std::vector<TeamMember> team = make_team(m_k);
            for (std::size_t i = 0; i < m_k; ++i) {
                auto future = std::apply(
                    [&](Args&... args) { return std::invoke(m_fn, std::move(team[i]), args...); },
                    m_args);
                m_set.build_task(std::move(future))
                    .name("parallel[" + std::to_string(i) + "]")
                    .spawn();
            }
        } catch (...) {
            // The parts already spawned would wait in a collective for the ones that
            // never started. cancel_all(), not m_drain->cancel(): the drain must still
            // end in Ready or PollError so that poll() reports this exception.
            m_spawn_error = std::current_exception();
            m_set.cancel_all();
        }
    }

    std::size_t                 m_k;
    F                           m_fn;
    std::tuple<Args...>         m_args;
    JoinSet<void>               m_set;
    std::optional<detail::JoinSetDrainFuture<void, void>> m_drain;
    std::exception_ptr          m_spawn_error;
    bool                        m_cancel_requested = false;
    bool                        m_finished         = false;
};

/**
 * @brief Runs `fn(member, args...)` for `k` parts, each as its own task, and completes
 * when all have finished.
 *
 * `fn` is a function or a lambda returning a `Future<void>` (normally `Coro<void>`),
 * whose first parameter is a TeamMember taken by value. That is the supported pattern:
 * an `fn` that keeps its member some other way isn't stopped, but its lifetime is then
 * the caller's to get right. `args` are stored once in the returned future and each
 * part gets them as lvalue references; pass `std::ref(x)` to share the caller's object.
 * A part can wait for the others with `co_await member.sync()`, or combine values with
 * them through the other collectives of TeamMember.
 *
 * A capturing lambda coroutine is safe here, unlike when invoked directly (see
 * doc/guidelines.md, CS.3): the returned future holds the lambda, doesn't move it once
 * the parts have started, and outlives every part. All `k` parts share the one lambda
 * object, so its captures are shared as `args` are.
 *
 * The first exception cancels the other parts; it is rethrown once all have
 * finished. The TeamBroken or TeamRoundBroken it causes in other parts is not.
 *
 * ```cpp
 * co_await coro::parallel(coro::worker_count(), filter_part, std::ref(table));
 * ```
 *
 * WARNING: each part holds a worker while it computes. Keep parts short, or leave
 * a worker free for the I/O driver and other tasks; see doc/guidelines.md,
 * "Blocking Work".
 */
template<typename F, typename... Args>
    requires detail::ParallelFn<F, Args...>
[[nodiscard]] ParallelFuture<F, Args...> parallel(std::size_t k, F fn, Args... args) {
    return ParallelFuture<F, Args...>(k, std::move(fn), std::move(args)...);
}

} // namespace coro
