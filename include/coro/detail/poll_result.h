#pragma once

#include <cstdint>
#include <exception>
#include <expected>
#include <memory>

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
 * Backed by a manually tagged union rather than `std::variant`: this type is
 * reconstructed on every `co_await` poll (see `FutureAwaitable::await_ready()`/
 * `poll()`), and `std::variant`'s generic index/visitation machinery showed up
 * as real, non-trivial overhead there. Pending/Dropped carry no payload, so the
 * only thing that ever needs storage is `Result` — a plain enum tag plus one
 * union member is enough, with no need for `std::variant`'s "valueless by
 * exception" handling since the states here never need that recovery path.
 *
 * @tparam T The value type produced on successful completion.
 */
template<typename T>
class PollResult {
    // Use std::expected<T, std::exception_ptr> as the sole payload type to hold
    // both the ready value and the error state. This avoids the ill-formed
    // std::variant<..., T, std::exception_ptr, ...> when T = std::exception_ptr,
    // which would introduce duplicate types and fail to compile.
    using Result = std::expected<T, std::exception_ptr>;

    enum class State : std::uint8_t { Pending, HasResult, Dropped };

public:
    PollResult(PendingTag) noexcept : m_state(State::Pending) {}

    PollResult(T value) : m_state(State::HasResult) {
        std::construct_at(&m_storage.result, std::move(value));
    }

    PollResult(PollError err) noexcept : m_state(State::HasResult) {
        std::construct_at(&m_storage.result, std::unexpected(std::move(err.exception)));
    }

    PollResult(DroppedTag) noexcept : m_state(State::Dropped) {}

    PollResult(const PollResult& other) : m_state(other.m_state) {
        if (m_state == State::HasResult)
            std::construct_at(&m_storage.result, other.m_storage.result);
    }

    PollResult(PollResult&& other) noexcept(std::is_nothrow_move_constructible_v<Result>)
        : m_state(other.m_state) {
        if (m_state == State::HasResult)
            std::construct_at(&m_storage.result, std::move(other.m_storage.result));
    }

    PollResult& operator=(const PollResult& other) {
        if (this == &other) return *this;
        destroy_result();
        m_state = other.m_state;
        if (m_state == State::HasResult)
            std::construct_at(&m_storage.result, other.m_storage.result);
        return *this;
    }

    PollResult& operator=(PollResult&& other) noexcept(std::is_nothrow_move_constructible_v<Result>) {
        if (this == &other) return *this;
        destroy_result();
        m_state = other.m_state;
        if (m_state == State::HasResult)
            std::construct_at(&m_storage.result, std::move(other.m_storage.result));
        return *this;
    }

    ~PollResult() { destroy_result(); }

    bool isPending() const noexcept { return m_state == State::Pending; }                              ///< @brief True if the future is not yet ready.
    bool isReady()   const noexcept { return m_state == State::HasResult && m_storage.result.has_value(); }   ///< @brief True if the future completed successfully.
    bool isError()   const noexcept { return m_state == State::HasResult && !m_storage.result.has_value(); }  ///< @brief True if the future faulted.
    bool isDropped() const noexcept { return m_state == State::Dropped; }                               ///< @brief True if the future was cancelled and drained.

    T&       value() &       { return m_storage.result.value(); }            ///< @brief Returns the ready value (lvalue ref).
    const T& value() const & { return m_storage.result.value(); }            ///< @brief Returns the ready value (const lvalue ref).
    T        value() &&      { return std::move(m_storage.result).value(); } ///< @brief Moves the ready value out.

    /// @brief Returns the stored exception pointer. Only valid when `isError()` is true.
    std::exception_ptr error() const noexcept { return m_storage.result.error(); }

    /// @brief Rethrows the stored exception if `isError()` is true. No-op otherwise.
    void rethrowIfError() const {
        if (isError()) std::rethrow_exception(m_storage.result.error());
    }

private:
    void destroy_result() noexcept {
        if (m_state == State::HasResult) std::destroy_at(&m_storage.result);
    }

    union Storage {
        Storage() noexcept {}
        ~Storage() noexcept {}
        Result result;
    };

    Storage m_storage;
    State   m_state;
};

/**
 * @brief `PollResult` specialization for futures that produce no value (`void`).
 *
 * Identical semantics to `PollResult<T>` but has no `value()` method.
 * Use `PollReady` to construct the ready state.
 */
template<>
class PollResult<void> {
    // Same approach as PollResult<T>: a single std::expected<void, exception_ptr>
    // payload covers both ready and error states, avoiding any duplicate-type issues.
    using Result = std::expected<void, std::exception_ptr>;

    enum class State : std::uint8_t { Pending, HasResult, Dropped };

public:
    /// @brief Tag type used to represent the Ready state for void futures.
    struct ReadyTag {};

    PollResult(PendingTag) noexcept : m_state(State::Pending) {}

    PollResult(ReadyTag) noexcept : m_state(State::HasResult) {
        std::construct_at(&m_storage.result);
    }

    PollResult(PollError err) noexcept : m_state(State::HasResult) {
        std::construct_at(&m_storage.result, std::unexpected(std::move(err.exception)));
    }

    PollResult(DroppedTag) noexcept : m_state(State::Dropped) {}

    PollResult(const PollResult& other) : m_state(other.m_state) {
        if (m_state == State::HasResult)
            std::construct_at(&m_storage.result, other.m_storage.result);
    }

    PollResult(PollResult&& other) noexcept(std::is_nothrow_move_constructible_v<Result>)
        : m_state(other.m_state) {
        if (m_state == State::HasResult)
            std::construct_at(&m_storage.result, std::move(other.m_storage.result));
    }

    PollResult& operator=(const PollResult& other) {
        if (this == &other) return *this;
        destroy_result();
        m_state = other.m_state;
        if (m_state == State::HasResult)
            std::construct_at(&m_storage.result, other.m_storage.result);
        return *this;
    }

    PollResult& operator=(PollResult&& other) noexcept(std::is_nothrow_move_constructible_v<Result>) {
        if (this == &other) return *this;
        destroy_result();
        m_state = other.m_state;
        if (m_state == State::HasResult)
            std::construct_at(&m_storage.result, std::move(other.m_storage.result));
        return *this;
    }

    ~PollResult() { destroy_result(); }

    bool isPending() const noexcept { return m_state == State::Pending; }
    bool isReady()   const noexcept { return m_state == State::HasResult && m_storage.result.has_value(); }
    bool isError()   const noexcept { return m_state == State::HasResult && !m_storage.result.has_value(); }
    bool isDropped() const noexcept { return m_state == State::Dropped; }

    /// @brief Returns the stored exception pointer. Only valid when `isError()` is true.
    std::exception_ptr error() const noexcept { return m_storage.result.error(); }

    /// @brief Rethrows the stored exception if `isError()` is true. No-op otherwise.
    void rethrowIfError() const {
        if (isError()) std::rethrow_exception(m_storage.result.error());
    }

private:
    void destroy_result() noexcept {
        if (m_state == State::HasResult) std::destroy_at(&m_storage.result);
    }

    union Storage {
        Storage() noexcept {}
        ~Storage() noexcept {}
        Result result;
    };

    Storage m_storage;
    State   m_state;
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
