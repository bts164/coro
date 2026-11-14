#pragma once

// ReadinessFuture: the future AsyncFd::read_io()/write_io() and
// ReadinessSource::read_io()/write_io() return. See doc/design/io_driver.md,
// "External drivers".
//
// Include coro/io/async_fd.h or coro/io/readiness_source.h rather than this header.

#include <coro/detail/context.h>
#include <coro/detail/isr_wait_core.h>
#include <coro/detail/mutex.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/rc.h>
#include <coro/detail/sys/poller.h>
#include <coro/future.h>
#include <coro/runtime/io_driver.h>

#include <cassert>
#include <cstddef>
#include <exception>
#include <expected>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace coro {

class AsyncFd;
class ReadinessSource;

namespace detail {

/**
 * @brief The current Runtime's IoDriver, for an external-driver object registering
 * with it.
 * @throws std::logic_error naming `what` outside a runtime, or if the runtime's
 *         executor never turns the driver (nothing could ever wake a waiter).
 */
IoDriver& external_io_driver(const char* what);

template<typename R>
struct IsErrnoExpected : std::false_type {};
template<typename T>
struct IsErrnoExpected<std::expected<T, int>> : std::true_type {};

/// An operation read_io()/write_io() accepts: callable with no arguments, returning
/// `std::expected<T, int>`.
template<typename Op>
concept ReadinessOp = std::is_invocable_v<Op&> &&
                      IsErrnoExpected<std::remove_cvref_t<std::invoke_result_t<Op&>>>::value;

/**
 * @brief One direction of an AsyncFd or a ReadinessSource: where its waiters' shared
 * registration comes from.
 *
 * Two kinds:
 * - **Fixed** (AsyncFd): the registration exists for the owner's whole life, and
 *   acquire() hands out shares of it.
 * - **Lazy** (ReadinessSource): a notify fd, registered while the direction has
 *   waiters. acquire() returns the existing registration if a waiter holds one, or
 *   registers the fd with the current runtime's driver; the last waiter's release()
 *   deregisters it. See doc/design/io_driver.md, "`ReadinessSource`".
 *
 * Not movable: futures hold its address.
 */
class ReadinessDirection {
public:
    /// `wait_direction` is the direction waiters wait in on the registration: the
    /// real one for an AsyncFd, always Read for a notify fd (a signal makes it
    /// readable).
    ReadinessDirection(IoDirection wait_direction, const char* what) noexcept
        : m_wait_direction(wait_direction), m_what(what) {}
    ~ReadinessDirection();

    ReadinessDirection(const ReadinessDirection&)            = delete;
    ReadinessDirection& operator=(const ReadinessDirection&) = delete;

    /// Makes this a fixed direction, sharing `registration`. Before any future exists.
    void set_fixed(Rc<NotifyRegistration> registration) noexcept;
    /// Makes this a lazy direction on notify fd `fd`. Before any future exists.
    void set_notify_fd(sys::RawFd fd) noexcept { m_fd = fd; }

    IoDirection wait_direction() const noexcept { return m_wait_direction; }
    const char* what() const noexcept { return m_what; }

    /// A share of the registration for a waiter that is about to wait.
    /// @throws std::logic_error outside a runtime, or if the runtime's executor does
    ///         not turn its driver (lazy only); std::system_error or std::bad_alloc if
    ///         the fd cannot be registered.
    Rc<NotifyRegistration> acquire();

    /// Drops `core`'s share, under the lock acquire() runs under. See
    /// IsrWaitCore::reset().
    void release(IsrWaitCore& core) noexcept;

    /// True while a registration exists (always, for a fixed direction that has one).
    bool registered() const;

    /// Debug bookkeeping of the futures that refer to this direction, so destroying
    /// or moving the owner while one exists asserts. No-ops under NDEBUG.
    void future_attached() noexcept;
    void future_detached() noexcept;
    bool has_futures() const noexcept;

private:
    bool registered_lazily() const;

    // GUARDS m_lazy, and the drop of the last share in release(): on the desktop two
    // first waiters on different executor threads must not register the same fd
    // twice, and a new waiter must not register it while the last one is still
    // deregistering it. A no-op on Pico (detail/mutex.h).
    mutable Mutex            m_mutex;
    IoDirection              m_wait_direction;
    const char*              m_what;
    Rc<NotifyRegistration>   m_fixed;                    // fixed only; set before use
    sys::RawFd               m_fd = sys::kInvalidFd;     // lazy only; never changes after
    Weak<NotifyRegistration> m_lazy;                     // lazy only. GUARDED BY m_mutex
#ifndef NDEBUG
    std::size_t              m_futures = 0;              // GUARDED BY m_mutex
#endif
};

} // namespace detail

/**
 * @brief The future AsyncFd and ReadinessSource return from read_io() and
 * write_io(): waits until `op()` succeeds in one direction.
 *
 * `op` is called on the executor's thread, once on the first poll and once after
 * each wake. It must not block or suspend, and it returns `std::expected<T, int>`:
 *   - a value: the future yields it;
 *   - a would-block errno (EAGAIN or EWOULDBLOCK): the future waits for readiness in
 *     its direction, then calls `op` again;
 *   - any other errno: the future fails with a `std::system_error`.
 * An exception `op` throws fails the future with that exception.
 *
 * Hand-written, like the socket futures, so an operation that succeeds on its first
 * poll costs no coroutine frame. A hand-written future can embed one as a member and
 * forward its poll().
 *
 * WARNING: pinned from its first poll: its IoWaiter is linked into the driver's
 * waiter list by address. The move operations only assert that it has not started.
 *
 * Refers to its AsyncFd or ReadinessSource, which must outlive it.
 */
template<detail::ReadinessOp Op>
class ReadinessFuture {
    using Result = std::remove_cvref_t<std::invoke_result_t<Op&>>;

public:
    using OutputType = typename Result::value_type;

    ReadinessFuture(ReadinessFuture&& other) noexcept(std::is_nothrow_move_constructible_v<Op>)
        : m_direction(std::exchange(other.m_direction, nullptr)),
          m_op(std::move(other.m_op)),
          m_core(std::move(other.m_core)) {}

    // Only for an operation that is itself move-assignable (a capturing lambda is
    // not).
    ReadinessFuture& operator=(ReadinessFuture&& other) noexcept
        requires std::is_nothrow_move_assignable_v<Op>
    {
        if (this != &other) {
            assert(!m_core.started() && "ReadinessFuture assigned to after its first poll()");
            detach();
            m_direction = std::exchange(other.m_direction, nullptr);
            m_op        = std::move(other.m_op);
            m_core      = std::move(other.m_core);
        }
        return *this;
    }

    ReadinessFuture(const ReadinessFuture&)            = delete;
    ReadinessFuture& operator=(const ReadinessFuture&) = delete;

    ~ReadinessFuture() { detach(); }

    PollResult<OutputType> poll(detail::Context& ctx) {
        assert(m_direction && "ReadinessFuture polled after being moved from");
        std::optional<Result> result;
        try {
            result = m_core.poll_io(ctx, m_direction->wait_direction(),
                                    [this] { return m_direction->acquire(); }, m_op);
        } catch (...) {
            return PollError(std::current_exception());
        }
        if (!result) return PollPending;
        if (!*result)
            return PollError(std::make_exception_ptr(
                std::system_error(result->error(), std::generic_category(), m_direction->what())));
        if constexpr (std::is_void_v<OutputType>)
            return PollReady;
        else
            return std::move(**result);
    }

private:
    friend class AsyncFd;
    friend class ReadinessSource;

    ReadinessFuture(detail::ReadinessDirection& direction, Op op)
        : m_direction(&direction), m_op(std::move(op)) {
        m_direction->future_attached();
    }

    void detach() noexcept {
        if (!m_direction) return;
        m_direction->release(m_core);
        m_direction->future_detached();
        m_direction = nullptr;
    }

    detail::ReadinessDirection* m_direction;
    Op                          m_op;
    detail::IsrWaitCore         m_core;
};

} // namespace coro
