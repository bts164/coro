#pragma once

#include <coro/future.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/context.h>
#include <array>
#include <cstddef>
#include <exception>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace coro {

/**
 * @brief State of a single branch inside a `JoinFuture`.
 *
 * Branches begin `Active` and are polled until they produce a result. When an
 * error occurs in any branch, remaining `Active` branches are cancelled and
 * transition to `Draining`; they must produce `PollDropped` before the join
 * can deliver its error. A branch reaches `Done` either by completing normally
 * or by finishing the drain sequence.
 */
enum class JoinBranchState {
    Active,   ///< Branch is still running; polled every `JoinFuture::poll()` call.
    Draining, ///< Branch has been cancelled; polled until it returns `PollDropped`.
    Done      ///< Branch has finished (completed, errored, or fully drained).
};

/**
 * @brief Placeholder result type used for `void`-returning branches in `JoinFuture`.
 *
 * Because `std::tuple` cannot hold `void` elements, void branches contribute a
 * `VoidJoinBranch` in the output tuple instead.
 */
struct VoidJoinBranch {};

namespace detail {

/**
 * @brief Traits helper that maps a branch's `OutputType` to the concrete types
 *        stored in `JoinFuture::m_results`.
 *
 * - Non-void `T`: `OptionalType = std::optional<T>` (empty until the branch completes).
 * - `void`:       `OptionalType = bool` (false until the branch completes, then true).
 *
 * @tparam T The `OutputType` of a single branch future.
 */
template<typename T>
struct JoinBranchTraits {
    using Type         = T;
    using OptionalType = std::optional<T>;
};

template<>
struct JoinBranchTraits<void> {
    using Type         = VoidJoinBranch;
    using OptionalType = bool; ///< `true` once the void branch has completed.
};

} // namespace detail

/// @brief The stored result element type for a branch with `OutputType = T`.
template<typename T>
using JoinBranchType = typename detail::JoinBranchTraits<T>::Type;

/// @brief The in-flight storage type for a branch with `OutputType = T`.
template<typename T>
using JoinBranchOptionalType = typename detail::JoinBranchTraits<T>::OptionalType;


/**
 * @brief Future that drives multiple branches concurrently and resolves once
 *        **all** branches have completed successfully.
 *
 * All branches are polled in round-robin order on every `poll()` call. If any
 * branch faults, all remaining active branches are cancelled and drained before
 * the first exception is propagated. If all branches succeed, their results are
 * returned as a `std::tuple`.
 *
 * - `OutputType` is `std::tuple<JoinBranchType<T0>, JoinBranchType<T1>, ...>`.
 *   `void` branches contribute `VoidJoinBranch{}` as their tuple element.
 * - Prefer the @ref join factory function over constructing this directly.
 *
 * @tparam Fs The `Future` types for each branch.
 */
template<Future... Fs>
class JoinFuture
{
public:
    static constexpr std::size_t N = sizeof...(Fs);
    using OutputType = std::tuple<JoinBranchType<typename Fs::OutputType>...>;

    /**
     * @brief Constructs a `JoinFuture` from a pack of futures.
     *
     * All branches start in the `Active` state. No polling occurs until `poll()`
     * is called for the first time.
     */
    explicit JoinFuture(Fs... futures)
        : m_futures(std::move(futures)...)
        , m_states{}  // value-initialised: all Active (== 0)
        , m_results{}
    {}

    JoinFuture(JoinFuture&&) noexcept = default;
    JoinFuture& operator=(JoinFuture&&) noexcept = default;
    JoinFuture(const JoinFuture&) = delete;
    JoinFuture& operator=(const JoinFuture&) = delete;

    /**
     * @brief Advances the join operation by one scheduling step.
     *
     * On each call:
     * 1. If an error has already been recorded (or all results are ready),
     *    resume draining any cancelled branches.
     * 2. Otherwise, poll all `Active` branches in round-robin order.
     * 3. If polling reveals an error, cancel remaining active branches and
     *    begin draining them.
     * 4. If all branches are now done, either propagate the exception or return
     *    the result tuple.
     *
     * @return `PollPending` while branches are still running or draining;
     *         the complete result tuple on success; `PollError` on the first
     *         exception raised by any branch.
     */
    PollResult<OutputType> poll(detail::Context& ctx) {
        // Re-entry after a previous poll() returned Pending while draining.
        if (m_exception || all_results_ready(std::make_index_sequence<N>{})) {
            if (poll_draining(ctx, std::make_index_sequence<N>{}))
                return PollPending;
            if (m_exception)
                return PollError(m_exception);
            return get_results(std::make_index_sequence<N>{});
        }

        // Poll all active branches in round-robin order.
        for (std::size_t i = 0; i < N; ++i) {
            std::size_t idx = (m_poll_start + i) % N;
            dispatch_poll_active(ctx, idx, std::make_index_sequence<N>{});
        }
        m_poll_start = (m_poll_start + 1) % N;

        // Check whether all branches finished (success) or an error was raised.
        if (m_exception || all_results_ready(std::make_index_sequence<N>{})) {
            if (poll_draining(ctx, std::make_index_sequence<N>{}))
                return PollPending;
            if (m_exception)
                return PollError(m_exception);
            return get_results(std::make_index_sequence<N>{});
        }

        return PollPending;
    }

private:
    // -----------------------------------------------------------------------
    // Readiness check
    // -----------------------------------------------------------------------

    /// Returns true if every branch has stored its result (success path only).
    template<std::size_t... Is>
    bool all_results_ready(std::index_sequence<Is...>) {
        return (... && (bool)std::get<Is>(m_results));
    }

    // -----------------------------------------------------------------------
    // Result extraction
    // -----------------------------------------------------------------------

    /// Assembles and returns the output tuple by moving results out of storage.
    template<std::size_t... Is>
    OutputType get_results(std::index_sequence<Is...>) {
        return OutputType(get_result_one<Is>()...);
    }

    /// Extracts the result for branch `I`. For void branches returns `VoidJoinBranch{}`.
    template<std::size_t I>
    auto get_result_one() {
        using F = std::tuple_element_t<I, std::tuple<Fs...>>;
        if constexpr (std::is_void_v<typename F::OutputType>) {
            return VoidJoinBranch{};
        } else {
            return std::move(std::get<I>(m_results).value());
        }
    }

    // -----------------------------------------------------------------------
    // Active-branch polling
    // -----------------------------------------------------------------------

    /// Dispatches a `poll_active_one<I>` call for the branch at runtime index `idx`.
    template<std::size_t... Is>
    void dispatch_poll_active(detail::Context& ctx, std::size_t idx, std::index_sequence<Is...>) {
        ((idx == Is ? poll_active_one<Is>(ctx) : void()), ...);
    }

    /**
     * @brief Polls a single active branch.
     *
     * If the branch becomes ready, its result is stored and its state is set to
     * `Done`. If it faults, the exception is recorded, the branch is marked `Done`,
     * and all remaining active branches are immediately cancelled so that draining
     * can begin.
     */
    template<std::size_t I>
    void poll_active_one(detail::Context& ctx) {
        if (m_states[I] != JoinBranchState::Active) return;

        auto result = std::get<I>(m_futures).poll(ctx);

        if (result.isPending()) return;

        m_states[I] = JoinBranchState::Done;

        if (result.isError()) {
            m_exception = result.error();
            // Cancel all remaining active branches so draining can begin.
            cancel_active(std::make_index_sequence<N>{});
        } else if (result.isDropped()) {
            // An active branch returned Dropped — not expected in normal usage;
            // state was already set to Done so nothing to do.
        } else {
            using F = std::tuple_element_t<I, std::tuple<Fs...>>;
            if constexpr (std::is_void_v<typename F::OutputType>) {
                std::get<I>(m_results) = true;
            } else {
                std::get<I>(m_results).emplace(std::move(result).value());
            }
        }
    }

    // -----------------------------------------------------------------------
    // Draining
    // -----------------------------------------------------------------------

    /**
     * @brief Polls all branches currently in the `Draining` state.
     * @return `true` if at least one branch is still draining after this pass.
     */
    template<std::size_t... Is>
    bool poll_draining(detail::Context& ctx, std::index_sequence<Is...>) {
        bool any_draining = false;
        ((poll_draining_one<Is>(ctx, any_draining)), ...);
        return any_draining;
    }

    /**
     * @brief Polls a single draining branch.
     *
     * Sets the branch to `Done` once it returns `PollDropped`. Any other result
     * (Ready, Error) is ignored — a cancellation acknowledge (`PollDropped`) is
     * the only accepted terminal transition while draining.
     */
    template<std::size_t I>
    void poll_draining_one(detail::Context& ctx, bool& any_still_draining) {
        if (m_states[I] != JoinBranchState::Draining) return;
        auto result = std::get<I>(m_futures).poll(ctx);
        if (result.isDropped())
            m_states[I] = JoinBranchState::Done;
        else
            any_still_draining = true;
    }

    // -----------------------------------------------------------------------
    // Cancellation
    // -----------------------------------------------------------------------

    /// Cancels every branch that is still `Active`.
    template<std::size_t... Is>
    void cancel_active(std::index_sequence<Is...>) {
        (cancel_one<Is>(), ...);
    }

    /**
     * @brief Cancels branch `I` if it is still `Active`.
     *
     * If the branch satisfies `Cancellable`, `cancel()` is called and the state
     * transitions to `Draining` (waiting for `PollDropped`). Otherwise the branch
     * is immediately marked `Done` — non-cancellable futures cannot be drained.
     */
    template<std::size_t I>
    void cancel_one() {
        if (m_states[I] != JoinBranchState::Active) return;
        if constexpr (Cancellable<std::tuple_element_t<I, std::tuple<Fs...>>>) {
            std::get<I>(m_futures).cancel();
            m_states[I] = JoinBranchState::Draining;
        } else {
            m_states[I] = JoinBranchState::Done;
        }
    }

    // -----------------------------------------------------------------------
    // Member data
    // -----------------------------------------------------------------------

    std::tuple<Fs...>                                                    m_futures;
    std::array<JoinBranchState, N>                                       m_states;
    std::tuple<JoinBranchOptionalType<typename Fs::OutputType>...>       m_results;
    std::exception_ptr                                                   m_exception;
    std::size_t                                                          m_poll_start = 0;
};


/**
 * @brief Drives multiple futures concurrently and returns all of their results.
 *
 * All branches are polled every scheduling step. The returned future resolves
 * once **every** branch has completed. If any branch faults, remaining branches
 * are cancelled and drained before the first exception is re-raised.
 *
 * `void`-returning branches contribute `VoidJoinBranch{}` in the output tuple.
 *
 * @param futures Two or more futures to run concurrently. Each must satisfy @ref Future.
 * @return A `JoinFuture` whose `OutputType` is
 *         `std::tuple<JoinBranchType<T0>, JoinBranchType<T1>, ...>`.
 *
 * Example:
 * @code
 * auto [a, b] = co_await join(fetch_int(), fetch_string());
 * @endcode
 */
template<Future... Fs>
[[nodiscard]] JoinFuture<std::remove_cvref_t<Fs>...> join(Fs&&... futures) {
    return JoinFuture<std::remove_cvref_t<Fs>...>(std::forward<Fs>(futures)...);
}

} // namespace coro
