#pragma once

#include <exception>
#include <variant>

namespace coro {

/// @brief Sentinel type for the Pending state. Use the `PollPending` constant.
struct PendingTag {};

/// @brief Returned by `poll()` to indicate the future is not yet ready. Waker has been registered.
inline constexpr PendingTag PollPending{};

/// @brief Sentinel type for the Dropped (cancelled + drained) state. Use the `PollDropped` constant.
struct DroppedTag {};

template<typename T>
class PollResult;

/**
 * @brief Helper for constructing error results: `return PollError(exception_ptr)`.
 *
 * Implicitly converts to any `PollResult<T>`, avoiding the need to spell out the full type.
 */
struct PollError {
    explicit PollError(std::exception_ptr e) : exception(std::move(e)) {}
    std::exception_ptr exception;

    template<typename T>
    operator PollResult<T>() const;
};

/**
 * @brief The result of a single `poll()` call on a `Future<T>`.
 *
 * Holds one of four states:
 * | State     | Meaning                                                          |
 * |-----------|------------------------------------------------------------------|
 * | `Pending` | Not ready; the waker from `Context` has been registered.         |
 * | `Ready`   | Completed successfully; value is accessible via `value()`.       |
 * | `Error`   | Faulted; exception is accessible via `error()` / `rethrowIfError()`. |
 * | `Dropped` | Cancelled and fully drained; propagates up the call chain.       |
 *
 * Constructing convenience:
 * - `return PollPending;` — pending state.
 * - `return value;` — ready state (implicit conversion).
 * - `return PollError(ex);` — error state.
 * - `return PollDropped;` — dropped state.
 *
 * @tparam T The value type produced on successful completion.
 */
template<typename T>
class PollResult {
public:
    PollResult(PendingTag) noexcept
        : m_state(std::in_place_type<PendingTag>) {}

    PollResult(T value)
        : m_state(std::in_place_type<T>, std::move(value)) {}

    PollResult(PollError err) noexcept
        : m_state(std::in_place_type<std::exception_ptr>, std::move(err.exception)) {}

    PollResult(DroppedTag) noexcept
        : m_state(std::in_place_type<DroppedTag>) {}

    bool isPending() const noexcept { return std::holds_alternative<PendingTag>(m_state); }  ///< @brief True if the future is not yet ready.
    bool isReady()   const noexcept { return std::holds_alternative<T>(m_state); }           ///< @brief True if the future completed successfully.
    bool isError()   const noexcept { return std::holds_alternative<std::exception_ptr>(m_state); } ///< @brief True if the future faulted.
    bool isDropped() const noexcept { return std::holds_alternative<DroppedTag>(m_state); }  ///< @brief True if the future was cancelled and drained.

    T&       value() &       { return std::get<T>(m_state); }            ///< @brief Returns the ready value (lvalue ref).
    const T& value() const & { return std::get<T>(m_state); }            ///< @brief Returns the ready value (const lvalue ref).
    T        value() &&      { return std::get<T>(std::move(m_state)); } ///< @brief Moves the ready value out.

    /// @brief Returns the stored exception pointer. Only valid when `isError()` is true.
    std::exception_ptr error() const noexcept {
        return std::get<std::exception_ptr>(m_state);
    }

    /// @brief Rethrows the stored exception if `isError()` is true. No-op otherwise.
    void rethrowIfError() const {
        if (isError()) std::rethrow_exception(std::get<std::exception_ptr>(m_state));
    }

private:
    std::variant<PendingTag, T, std::exception_ptr, DroppedTag> m_state;
};

/**
 * @brief `PollResult` specialization for futures that produce no value (`void`).
 *
 * Identical semantics to `PollResult<T>` but has no `value()` method.
 * Use `PollReady` to construct the ready state.
 */
template<>
class PollResult<void> {
public:
    /// @brief Tag type used to represent the Ready state for void futures.
    struct ReadyTag {};

    PollResult(PendingTag) noexcept
        : m_state(std::in_place_type<PendingTag>) {}

    PollResult(ReadyTag) noexcept
        : m_state(std::in_place_type<ReadyTag>) {}

    PollResult(PollError err) noexcept
        : m_state(std::in_place_type<std::exception_ptr>, std::move(err.exception)) {}

    PollResult(DroppedTag) noexcept
        : m_state(std::in_place_type<DroppedTag>) {}

    bool isPending() const noexcept { return std::holds_alternative<PendingTag>(m_state); }
    bool isReady()   const noexcept { return std::holds_alternative<ReadyTag>(m_state); }
    bool isError()   const noexcept { return std::holds_alternative<std::exception_ptr>(m_state); }
    bool isDropped() const noexcept { return std::holds_alternative<DroppedTag>(m_state); }

    /// @brief Returns the stored exception pointer. Only valid when `isError()` is true.
    std::exception_ptr error() const noexcept {
        return std::get<std::exception_ptr>(m_state);
    }

    /// @brief Rethrows the stored exception if `isError()` is true. No-op otherwise.
    void rethrowIfError() const {
        if (isError()) std::rethrow_exception(std::get<std::exception_ptr>(m_state));
    }

private:
    std::variant<PendingTag, ReadyTag, std::exception_ptr, DroppedTag> m_state;
};

/// @brief Sentinel for constructing a Ready void result: `return PollReady;`
inline constexpr PollResult<void>::ReadyTag PollReady{};

/// @brief Sentinel for constructing a Dropped result: `return PollDropped;`
inline constexpr DroppedTag PollDropped{};

// Defined after PollResult is complete to resolve the forward declaration.
template<typename T>
inline PollError::operator PollResult<T>() const {
    return PollResult<T>(*this);
}

} // namespace coro
