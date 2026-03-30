#pragma once

#include <exception>
#include <variant>

namespace coro {

struct PendingTag {};
inline constexpr PendingTag PollPending{};

struct DroppedTag {};

template<typename T>
class PollResult;

// Helper for constructing error states: return PollError(exception_ptr);
// Implicitly converts to any PollResult<T>.
struct PollError {
    explicit PollError(std::exception_ptr e) : exception(std::move(e)) {}
    std::exception_ptr exception;

    template<typename T>
    operator PollResult<T>() const;
};

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

    bool isPending() const noexcept { return std::holds_alternative<PendingTag>(m_state); }
    bool isReady()   const noexcept { return std::holds_alternative<T>(m_state); }
    bool isError()   const noexcept { return std::holds_alternative<std::exception_ptr>(m_state); }
    bool isDropped() const noexcept { return std::holds_alternative<DroppedTag>(m_state); }

    T&       value() &       { return std::get<T>(m_state); }
    const T& value() const & { return std::get<T>(m_state); }
    T        value() &&      { return std::get<T>(std::move(m_state)); }

    std::exception_ptr error() const noexcept {
        return std::get<std::exception_ptr>(m_state);
    }

    void rethrowIfError() const {
        if (isError()) std::rethrow_exception(std::get<std::exception_ptr>(m_state));
    }

private:
    std::variant<PendingTag, T, std::exception_ptr, DroppedTag> m_state;
};

// Specialization for void — no value() method; Ready state uses a tag.
template<>
class PollResult<void> {
public:
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

    std::exception_ptr error() const noexcept {
        return std::get<std::exception_ptr>(m_state);
    }

    void rethrowIfError() const {
        if (isError()) std::rethrow_exception(std::get<std::exception_ptr>(m_state));
    }

private:
    std::variant<PendingTag, ReadyTag, std::exception_ptr, DroppedTag> m_state;
};

// Sentinel for constructing a Ready void result: return PollReady;
inline constexpr PollResult<void>::ReadyTag PollReady{};

// Sentinel for constructing a Dropped result: return PollDropped;
inline constexpr DroppedTag PollDropped{};

// Defined after PollResult is complete to resolve the forward declaration.
template<typename T>
inline PollError::operator PollResult<T>() const {
    return PollResult<T>(*this);
}

} // namespace coro
