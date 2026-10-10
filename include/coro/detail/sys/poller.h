#pragma once

// Platform readiness layer: coro's internal equivalent of mio.
//
// Everything above this header (IoDriver, IoRegistration, the I/O primitives) is
// platform-independent. Porting coro's I/O to a new OS means providing these few
// types and nothing else. See doc/design/io_driver.md, "The sys layer".
//
// Current backends:
//   Linux: epoll (src/detail/sys/poller_epoll.cpp)
//
// Contract every backend must provide:
//   - Edge-triggered delivery. An event is only guaranteed after readiness *changes*,
//     so a consumer may only wait after it has observed EAGAIN. (A level-triggered
//     backend still satisfies this; it just reports extra events.)
//   - Thread-safe registration. Any thread may register_fd()/reregister_fd()/
//     deregister_fd() while another thread is blocked in poll().
//   - A nanosecond-resolution poll() timeout, honored as precisely as the OS allows.

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>

namespace coro::detail::sys {

/// Native handle type for a pollable I/O object. `int` on POSIX; a Windows backend
/// would make this `SOCKET`.
using RawFd = int;

/// True if `err` (an errno value) means "the operation would block": the non-blocking
/// syscall found nothing to do, and the caller should wait for readiness.
inline bool would_block(int err) noexcept {
    return err == EAGAIN || err == EWOULDBLOCK;
}

/// The errno an I/O operation fails with when the driver its fd is registered with
/// has been shut down, so that it can never be woken for readiness again.
inline constexpr int kDriverShutDown = ECANCELED;

/// Which readiness directions a registration is interested in.
struct Interest {
    bool readable = false;
    bool writable = false;

    static constexpr Interest read()       { return {true, false}; }
    static constexpr Interest write()      { return {false, true}; }
    static constexpr Interest read_write() { return {true, true}; }
};

/// One readiness notification returned by Poller::poll().
struct Event {
    /// The opaque key the fd was registered with (IoDriver passes a ScheduledIo*).
    void* key = nullptr;
    bool  readable = false;
    bool  writable = false;
    /// An error is pending on the fd (EPOLLERR). Waiters in both directions should
    /// retry their syscall, which reports the error.
    bool  error = false;
    /// The peer hung up or the read side shut down (EPOLLHUP / EPOLLRDHUP).
    bool  hup = false;
};

/**
 * @brief OS readiness queue (epoll on Linux).
 *
 * register_fd(), reregister_fd() and deregister_fd() are thread-safe and may be
 * called while another thread is blocked in poll(). poll() itself must be called by
 * at most one thread at a time; IoDriver enforces this.
 */
class Poller {
public:
    Poller();
    ~Poller();

    Poller(const Poller&)            = delete;
    Poller& operator=(const Poller&) = delete;
    Poller(Poller&&)                 = delete;
    Poller& operator=(Poller&&)      = delete;

    /// Starts edge-triggered monitoring of `fd`. `key` is returned in each Event for
    /// this fd and must remain valid until the fd is deregistered AND the poll() batch
    /// that might still hold it has been processed (see IoDriver's pending-release list).
    /// @throws std::system_error on failure.
    void register_fd(RawFd fd, void* key, Interest interest);

    /// Replaces the key and interest of an already registered fd.
    /// @throws std::system_error on failure.
    void reregister_fd(RawFd fd, void* key, Interest interest);

    /// Stops monitoring `fd`. Must be called BEFORE the fd is closed: epoll watches the
    /// open file, not the fd number, so if the fd was closed while a dup of it is
    /// still open, the registration survives and its key can still be reported.
    /// Never throws, so it is safe to call from destructors.
    /// @return true if `fd` was registered and is now removed; false if it was not
    ///         registered, or is no longer a valid fd (closed too early, or invalid).
    bool deregister_fd(RawFd fd) noexcept;

    /// Waits for readiness events and appends them to `out` (which is not cleared).
    ///
    /// @param timeout `std::nullopt` waits indefinitely; zero returns immediately.
    /// Returns early, possibly with no events, if interrupted by a signal.
    /// @throws std::system_error on failure other than EINTR.
    void poll(std::vector<Event>& out, std::optional<std::chrono::nanoseconds> timeout);

private:
    RawFd m_fd = -1;  // the epoll instance
};

/**
 * @brief Wakes a thread blocked in Poller::poll() from any thread.
 *
 * Linux: an eventfd registered (readable) with the poller under a caller-chosen key.
 * When poll() returns an Event carrying that key, the driver calls reset().
 */
class PollWaker {
public:
    /// Registers the waker with `poller` under `key`. `poller` must outlive the waker.
    PollWaker(Poller& poller, void* key);
    ~PollWaker();

    PollWaker(const PollWaker&)            = delete;
    PollWaker& operator=(const PollWaker&) = delete;
    PollWaker(PollWaker&&)                 = delete;
    PollWaker& operator=(PollWaker&&)      = delete;

    /// Makes the current or next poll() return. Thread-safe. A wake() issued while no
    /// thread is in poll() is not lost: the next poll() returns immediately.
    void wake() noexcept;

    /// Consumes pending wakes. Call from the polling thread after poll() returned this
    /// waker's key. No-op on backends that don't need it.
    void reset() noexcept;

private:
    Poller& m_poller;
    RawFd   m_fd = -1;  // eventfd
};

} // namespace coro::detail::sys
