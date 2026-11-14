#pragma once

// AsyncFd: a file descriptor that code outside coro reads and writes itself, with the
// waiting done by the runtime's IoDriver. tokio's AsyncFd. See
// doc/design/io_driver.md, "External drivers" and "`AsyncFd`".

#if defined(_WIN32)
#error "coro/io/async_fd.h is not available on Windows: the IOCP poller can only poll sockets (see doc/design/io_driver.md, \"AsyncFd\")"
#elif defined(CORO_SYS_LWIP)
#error "coro/io/async_fd.h is not available on Pico, which has no file descriptors; use coro/io/readiness_source.h"
#endif

#include <coro/detail/sys/poller.h>
#include <coro/io/readiness_future.h>

#include <cassert>
#include <memory>
#include <utility>

namespace coro {

/// The readiness directions an AsyncFd registers for.
using Interest = detail::sys::Interest;

/**
 * @brief Owns a file descriptor and registers it, edge-triggered, with the current
 * runtime's IoDriver, so code outside coro can wait for it to become readable or
 * writable: a character device, a GPIO line-event fd, a serial port, a socket from a
 * C library.
 *
 * The caller performs the I/O itself, in an operation passed to read_io() or
 * write_io(); see ReadinessFuture for its contract. Errors and hang-ups wake waiters
 * in both directions, and the operation's own syscall then reports them.
 *
 * Registered for its whole life, so it belongs to the runtime it was created in:
 * destroy it before that runtime. Must outlive every future it returns.
 */
class AsyncFd {
public:
    /// Takes ownership of `fd`, sets O_NONBLOCK on it, and registers it,
    /// edge-triggered, with the current runtime's driver for `interest`. If this
    /// throws, `fd` has been closed.
    /// @throws std::logic_error outside a runtime, or if the runtime's executor does
    ///         not turn its driver; std::system_error if the OS rejects the fd (a
    ///         regular file, for instance, which epoll refuses with EPERM).
    explicit AsyncFd(int fd, Interest interest = Interest::read_write());

    /// Deregisters, then closes the fd. Precondition: no future of it exists.
    ~AsyncFd();

    /// Only while no future of `other` exists. Leaves `other` empty.
    AsyncFd(AsyncFd&& other) noexcept;
    /// Only while no future of either exists. Closes this one's fd first.
    AsyncFd& operator=(AsyncFd&& other) noexcept;

    AsyncFd(const AsyncFd&)            = delete;
    AsyncFd& operator=(const AsyncFd&) = delete;

    /// The fd, for the operation to call read(), write(), ioctl() and so on on; -1 if
    /// empty. Do not close it: use release().
    int fd() const noexcept;

    /// Deregisters and gives the fd back, open (and still non-blocking). Returns -1 if
    /// empty. The AsyncFd is empty afterwards. Precondition: no future of it exists.
    int release() noexcept;

    /// Waits until `op()` succeeds, calling it again each time the fd becomes
    /// readable. See ReadinessFuture for `op`'s contract. Requires read interest.
    template<detail::ReadinessOp Op>
    [[nodiscard]] ReadinessFuture<Op> read_io(Op op) {
        assert(m_state && m_state->interest.readable && "AsyncFd::read_io without read interest");
        return ReadinessFuture<Op>(m_state->read, std::move(op));
    }

    /// Waits until `op()` succeeds, calling it again each time the fd becomes
    /// writable. Requires write interest.
    template<detail::ReadinessOp Op>
    [[nodiscard]] ReadinessFuture<Op> write_io(Op op) {
        assert(m_state && m_state->interest.writable && "AsyncFd::write_io without write interest");
        return ReadinessFuture<Op>(m_state->write, std::move(op));
    }

private:
    // On the heap, so the directions the futures point at stay put if the AsyncFd
    // moves (which is still allowed only while it has no futures).
    struct State {
        State(int fd_, Interest interest_) : fd(fd_), interest(interest_) {}

        int                                    fd;
        Interest                               interest;
        detail::Rc<detail::NotifyRegistration> registration;
        detail::ReadinessDirection             read{IoDirection::Read, "AsyncFd::read_io"};
        detail::ReadinessDirection             write{IoDirection::Write, "AsyncFd::write_io"};
    };

    /// Deregisters and returns the fd (still open), leaving this empty.
    int take() noexcept;

    std::unique_ptr<State> m_state;
};

} // namespace coro
