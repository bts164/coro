#pragma once

// The waiting half shared by everything that lets any number of tasks wait on a
// registration they acquire lazily: the ISR primitives (coro/sync/isr_event.h) and
// the external-driver types (coro/io/readiness_source.h, coro/io/async_fd.h).
//
// Platform-neutral: the ISR primitives are Pico only, but ReadinessSource and AsyncFd
// are built on the same core on the desktop. See doc/design/io_driver.md, "External
// drivers", and doc/design/isr_safety.md, "The wait".

#include <coro/detail/context.h>
#include <coro/detail/rc.h>
#include <coro/detail/sys/poller.h>
#include <coro/runtime/io_driver.h>

#include <cassert>
#include <expected>
#include <optional>
#include <type_traits>
#include <utility>

namespace coro::detail {

/// A registration shared by every waiter of one object while it has any: an ISR
/// primitive's or a ReadinessSource direction's notify fd, or an AsyncFd's fd. See
/// doc/design/isr_safety.md, "The wait".
struct NotifyRegistration {
    NotifyRegistration(IoDriver& driver, sys::RawFd fd, sys::Interest interest = sys::Interest::read())
        : reg(driver, fd, interest) {}

    IoRegistration reg;
};

/**
 * @brief The waiting half every waiter-list future shares: a share of the object's
 * registration and the future's own IoWaiter.
 *
 * WARNING: the future is pinned from its first poll (doc/design/future_and_stream.md):
 * its IoWaiter is linked into the driver's list by address. The move operations only
 * assert that it has not started; they never re-link.
 */
class IsrWaitCore {
public:
    IsrWaitCore() = default;
    IsrWaitCore([[maybe_unused]] IsrWaitCore&& other) noexcept {
        assert(!other.m_registration && "wait future moved after its first poll()");
    }
    IsrWaitCore& operator=([[maybe_unused]] IsrWaitCore&& other) noexcept {
        assert(!m_registration && "wait future assigned to after its first poll()");
        assert(!other.m_registration && "wait future moved after its first poll()");
        return *this;
    }
    IsrWaitCore(const IsrWaitCore&)            = delete;
    IsrWaitCore& operator=(const IsrWaitCore&) = delete;

    ~IsrWaitCore() { reset(); }

    /// True once this waiter holds a share of the registration.
    bool started() const noexcept { return static_cast<bool>(m_registration); }

    /**
     * @brief Unlinks the waiter and drops this waiter's share of the registration.
     * The last share to go deregisters. Idempotent.
     *
     * An owner whose registration can be acquired from several threads (a
     * ReadinessSource on the desktop) calls this under the lock its acquire runs
     * under, so a new registration is never made while the old one is still being
     * deregistered.
     */
    void reset() noexcept {
        if (!m_registration) return;
        m_registration->reg.cancel_wait(m_waiter);
        m_registration = {};
    }

    /**
     * @brief One poll of a wait in `direction`.
     *
     * `op()` returns `std::expected<T, int>`. It runs first, before any registration
     * exists, so a state change made before the wait started is seen without
     * registering. If it would block, `acquire()` returns the registration to share
     * (it may throw, which this lets propagate), and the waiter waits on it through
     * the driver's waiter list; every wake runs `op()` again.
     *
     * @return std::nullopt while waiting; otherwise `op()`'s result: a value, or an
     *         errno other than would-block (sys::kDriverShutDown once the driver has
     *         been shut down).
     */
    template<typename Acquire, typename Op>
    auto poll_io(Context& ctx, IoDirection direction, Acquire&& acquire, Op& op)
        -> std::optional<std::invoke_result_t<Op&>>
    {
        if (!m_registration) {
            auto first = op();
            if (first || !sys::would_block(first.error())) return first;
            m_registration = acquire();
        }
        // Race (handled): a signal between op() above and the waiter being linked
        // cannot be dispatched in between (dispatch runs in the driver's turn, not in
        // a task poll), so its event finds the waiter linked. One that arrived before
        // the registration existed left a new registration ready, so poll_io() runs
        // op() again before waiting.
        return m_registration->reg.poll_io(direction, ctx, m_waiter, op);
    }

private:
    // Declared before m_waiter, so m_waiter (unlinked by reset()) is destroyed first.
    Rc<NotifyRegistration> m_registration;
    IoWaiter               m_waiter;
};

} // namespace coro::detail
