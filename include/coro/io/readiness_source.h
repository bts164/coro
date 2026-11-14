#pragma once

// ReadinessSource: readiness that a device driver's own code signals, for drivers
// outside coro. See doc/design/io_driver.md, "External drivers" and
// "`ReadinessSource`", and doc/design/isr_safety.md, "Device drivers:
// `ReadinessSource`".

#if defined(_WIN32)
#error "coro/io/readiness_source.h is not available on Windows: its backend has no notify seam (see doc/design/io_driver.md, \"ReadinessSource\")"
#endif

#include <coro/detail/sys/notify.h>
#include <coro/io/readiness_future.h>

#include <utility>

namespace coro {

namespace detail { struct ReadinessSourceProbe; }   // test access, test/io/test_readiness_source.cpp

/**
 * @brief Readiness signalled by a device driver's own code: an interrupt handler on
 * Pico, another thread on the desktop.
 *
 * The device keeps its state (a packet queue, a status register) and offers
 * non-blocking operations on it. When the state changes so that an operation may now
 * succeed, the device calls signal_readable() or signal_writable(). Tasks wait with
 * read_io()/write_io(), which loop "call the operation, wait for a signal, call it
 * again".
 *
 * The device's rules (doc/design/io_driver.md, "The device's rules"):
 *   1. Change the state, then signal.
 *   2. Signal after every change that can turn EAGAIN into success.
 *   3. Guard the state with the device's own lock, taken by both the signalling side
 *      and the operations.
 *   4. Signal after releasing that lock.
 *
 * A pair of notify fds (detail/sys/notify.h), one per direction, each registered with
 * the current runtime's driver only while that direction has waiters. It needs no
 * runtime to construct, so it may be a global. Must outlive every future it returns
 * and every thread or interrupt handler that signals it.
 */
class ReadinessSource {
public:
    /// Opens its two notify objects.
    /// @throws std::system_error or std::bad_alloc.
    ReadinessSource();

    /// Closes them. Precondition: no future of it exists.
    ~ReadinessSource();

    ReadinessSource(const ReadinessSource&)            = delete;
    ReadinessSource& operator=(const ReadinessSource&) = delete;

    /// Reports that a read operation may now succeed. Safe from any thread, and on
    /// Pico from an interrupt handler on either core. Never call it while holding a
    /// hardware spin lock: it may take one of its own.
    void signal_readable() noexcept;

    /// Reports that a write operation may now succeed. Same rules as
    /// signal_readable().
    void signal_writable() noexcept;

    /// Both directions: an error, or the device closing, which every waiter must see.
    void signal() noexcept;

    /// Waits until `op()` succeeds, calling it again after each signal_readable() or
    /// signal(). See ReadinessFuture for `op`'s contract. Waiters of either kind may
    /// be any number.
    template<detail::ReadinessOp Op>
    [[nodiscard]] ReadinessFuture<Op> read_io(Op op) {
        return ReadinessFuture<Op>(m_read, std::move(op));
    }

    /// Waits until `op()` succeeds, calling it again after each signal_writable() or
    /// signal().
    template<detail::ReadinessOp Op>
    [[nodiscard]] ReadinessFuture<Op> write_io(Op op) {
        return ReadinessFuture<Op>(m_write, std::move(op));
    }

private:
    friend struct detail::ReadinessSourceProbe;

    // Both fds are opened by the constructor and never change, so the signal
    // functions read them without a lock.
    detail::sys::RawFd         m_read_fd  = detail::sys::kInvalidFd;
    detail::sys::RawFd         m_write_fd = detail::sys::kInvalidFd;
    // Each direction has a notify fd of its own, and a signal makes that fd readable,
    // so the write direction's waiters wait for Read readiness too.
    detail::ReadinessDirection m_read{IoDirection::Read, "ReadinessSource::read_io"};
    detail::ReadinessDirection m_write{IoDirection::Read, "ReadinessSource::write_io"};
};

} // namespace coro
