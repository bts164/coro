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
#include <variant>

namespace coro {

/**
 * @brief Tagged result wrapper identifying which branch of a `select()` call won.
 *
 * `index` is the compile-time branch index (0-based). For non-void branches, `value`
 * holds the branch's result.
 *
 * @tparam N The compile-time branch index.
 * @tparam T The result type of the branch (`void` for branches with no value).
 */
template<std::size_t N, typename T>
struct SelectBranch {
    static constexpr std::size_t index = N;
    T value;
    explicit SelectBranch(T v) : value(std::move(v)) {}
};

/// @brief `SelectBranch` specialization for `void` branches. Carries only the index.
template<std::size_t N>
struct SelectBranch<N, void> {
    static constexpr std::size_t index = N;
};

namespace detail {

// Build std::variant<SelectBranch<0,T0>, SelectBranch<1,T1>, ...> from a pack of Futures.
template<typename IndexSeq, typename... Fs>
struct SelectOutputTypeHelper;

template<std::size_t... Is, typename... Fs>
struct SelectOutputTypeHelper<std::index_sequence<Is...>, Fs...> {
    using type = std::variant<SelectBranch<Is, typename Fs::OutputType>...>;
};

} // namespace detail


//! Branch state for a `select()` future.
enum class SelectBranchState : uint8_t { Active, Draining, Done };


/**
 * @brief Future that races multiple branches and returns the result of the first to complete.
 *
 * Polls branches in round-robin order to ensure fairness. When one branch completes
 * (Ready or Error), all other @ref Cancellable branches are cancelled and drained before
 * the result is delivered. Non-cancellable branches are dropped immediately.
 *
 * `OutputType` is `std::variant<SelectBranch<0,T0>, SelectBranch<1,T1>, ...>`.
 *
 * Prefer the @ref select factory function over constructing this directly.
 *
 * @tparam Fs The Future types for each branch.
 */
template<Future... Fs>
class SelectFuture {
    static constexpr std::size_t N = sizeof...(Fs);

public:
    using OutputType = typename detail::SelectOutputTypeHelper<
        std::make_index_sequence<N>, Fs...>::type;

    explicit SelectFuture(Fs... futures)
        : m_futures(std::move(futures)...)
        , m_states{}   // value-initialised: all Active (== 0)
    {}

    SelectFuture(SelectFuture&&) noexcept = default;
    SelectFuture& operator=(SelectFuture&&) noexcept = default;
    SelectFuture(const SelectFuture&) = delete;
    SelectFuture& operator=(const SelectFuture&) = delete;

    PollResult<OutputType> poll(detail::Context& ctx) {
        // If a winner has already been decided, drain cancelled branches then deliver.
        if (m_result.has_value() || m_exception) {
            if (poll_draining(ctx, std::make_index_sequence<N>{}))
                return PollPending;
            if (m_exception)
                return PollError(m_exception);
            return std::move(*m_result);
        }

        // Poll active branches in round-robin order.
        for (std::size_t i = 0; i < N; ++i) {
            std::size_t idx = (m_poll_start + i) % N;
            if (dispatch_poll_active(ctx, idx, std::make_index_sequence<N>{}))
                break;
        }
        m_poll_start = (m_poll_start + 1) % N;

        // If a winner was found this poll, drain immediately in the same poll() call.
        if (m_result.has_value() || m_exception) {
            if (poll_draining(ctx, std::make_index_sequence<N>{}))
                return PollPending;
            if (m_exception)
                return PollError(m_exception);
            return std::move(*m_result);
        }

        return PollPending;
    }

private:
    std::tuple<Fs...>                          m_futures;
    std::array<SelectBranchState, N>           m_states;
    std::optional<OutputType>                  m_result;
    std::exception_ptr                         m_exception;
    std::size_t                                m_poll_start = 0;

    // --- Drain pass ---

    template<std::size_t... Is>
    bool poll_draining(detail::Context& ctx, std::index_sequence<Is...>) {
        bool any = false;
        ((poll_draining_one<Is>(ctx, any)), ...);
        return any;
    }

    template<std::size_t I>
    void poll_draining_one(detail::Context& ctx, bool& any_still_draining) {
        if (m_states[I] != SelectBranchState::Draining) return;
        auto r = std::get<I>(m_futures).poll(ctx);
        if (r.isDropped())
            m_states[I] = SelectBranchState::Done;
        else
            any_still_draining = true;
    }

    // --- Active-branch dispatch (compile-time index → runtime index) ---

    template<std::size_t... Is>
    bool dispatch_poll_active(detail::Context& ctx, std::size_t idx, std::index_sequence<Is...>) {
        bool found = false;
        ((idx == Is && !found ? (found = poll_active_one<Is>(ctx)) : false), ...);
        return found;
    }

    // Returns true if this branch became the winner.
    template<std::size_t I>
    bool poll_active_one(detail::Context& ctx) {
        if (m_states[I] != SelectBranchState::Active) return false;

        auto result = std::get<I>(m_futures).poll(ctx);

        if (result.isPending()) return false;

        // A non-pending result (Ready, Error, or Dropped) resolves this branch.
        m_states[I] = SelectBranchState::Done;

        if (result.isError()) {
            m_exception = result.error();
        } else if (result.isDropped()) {
            // An active branch returned Dropped — not expected in normal usage;
            // state was already set to Done so nothing to do
        } else {
            set_result<I>(std::move(result));
        }

        // Cancel all other active branches.
        cancel_others(I, std::make_index_sequence<N>{});
        return true;
    }

    // --- Result storage helpers ---

    template<std::size_t I>
    void set_result(PollResult<typename std::tuple_element_t<I, std::tuple<Fs...>>::OutputType> r) {
        using T = typename std::tuple_element_t<I, std::tuple<Fs...>>::OutputType;
        if constexpr (std::is_void_v<T>) {
            m_result.emplace(std::in_place_index<I>);
        } else {
            m_result.emplace(std::in_place_index<I>, SelectBranch<I, T>(std::move(r).value()));
        }
    }

    template<std::size_t I>
    void set_void_result() {
        m_result.emplace(std::in_place_index<I>);
    }

    // --- Cancel all branches except the winner ---

    template<std::size_t... Is>
    void cancel_others(std::size_t winner, std::index_sequence<Is...>) {
        ((Is != winner ? cancel_one<Is>() : void()), ...);
    }

    template<std::size_t I>
    void cancel_one() {
        if (m_states[I] != SelectBranchState::Active) return;
        if constexpr (Cancellable<std::tuple_element_t<I, std::tuple<Fs...>>>) {
            std::get<I>(m_futures).cancel();
            m_states[I] = SelectBranchState::Draining;
        } else {
            m_states[I] = SelectBranchState::Done;
        }
    }
};


/**
 * @brief Races multiple futures and returns the result of the first to complete.
 *
 * @param futures Two or more futures to race. Each must satisfy @ref Future.
 * @return A @ref SelectFuture whose `OutputType` is
 *         `std::variant<SelectBranch<0,T0>, SelectBranch<1,T1>, ...>`.
 *
 * Example:
 * @code
 * auto result = co_await select(fetch_data(), sleep_for(5s));
 * if (result.index() == 0)
 *     use(std::get<0>(result).value);  // fetch_data() won
 * else
 *     handle_timeout();                // sleep_for() won
 * @endcode
 */
template<Future... Fs>
[[nodiscard]] SelectFuture<std::remove_cvref_t<Fs>...> select(Fs&&... futures) {
    return SelectFuture<std::remove_cvref_t<Fs>...>(std::forward<Fs>(futures)...);
}

} // namespace coro
