#pragma once

// Platform readiness layer: coro's internal equivalent of mio.
//
// Everything above this header (IoDriver, IoRegistration, the I/O primitives) is
// platform-independent. Porting coro's I/O to a new OS means providing these few
// types and nothing else. See doc/design/io_driver.md, "The sys layer".
//
// Current backends:
//   Linux: epoll (src/detail/sys/posix/poller_epoll.cpp)
//   lwIP on the Pico SDK (CORO_SYS_LWIP): a ready list fed by lwIP's callbacks, run and
//     slept on the SDK's async_context (src/detail/sys/lwip/poller_lwip.cpp). See
//     doc/design/pico_port.md, "The lwIP sys backend".
//   Windows (CORO_SYS_WINDOWS): IOCTL_AFD_POLL requests completing on an I/O
//     completion port (src/detail/sys/windows/poller_iocp.cpp). See
//     doc/design/windows_port.md.
//
// Contract every backend must provide:
//   - Edge-triggered delivery (Trigger::Edge, the default). An event is only
//     guaranteed after readiness *changes*, so a consumer may only wait after it has
//     observed EAGAIN. (A level-triggered backend still satisfies this; it just
//     reports extra events.) A backend that emulates edges with one-shot polls
//     (Windows) reports a direction again only after rearm(), which the consumer
//     calls on EAGAIN (IoRegistration::clear_ready() does).
//   - Level-triggered delivery (Trigger::Level). Every poll() reports the fd for as
//     long as it is ready in a direction the registration is interested in. For a
//     consumer that cannot promise to drain the fd, such as a foreign library's
//     socket (see IoRegistration's level-triggered mode).
//   - Thread-safe registration. Any thread may register_fd()/reregister_fd()/
//     deregister_fd() while another thread is blocked in poll().
//   - A nanosecond-resolution poll() timeout, honored as precisely as the OS allows.

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#ifdef CORO_SYS_LWIP
// The Pico SDK's async_context (pico/async_context.h). Declared here so that this
// header, which public headers include, does not pull in the SDK.
struct async_context;
#elif defined(_WIN32)
// The Windows backend. Derived here rather than set by the build, so that a program
// including this header agrees with the library about it.
#define CORO_SYS_WINDOWS 1
#include <memory>
#include <mutex>
#endif

namespace coro::detail::sys {

#ifdef CORO_SYS_LWIP
/// The lwIP backend's analogue of an open file. Defined in
/// src/detail/sys/lwip/io_object.h; opaque everywhere else.
struct IoObject;

/// Native handle type for a pollable I/O object. On lwIP there is no boundary
/// between user space and a kernel for a number to cross, so the handle is the
/// object itself.
using RawFd = IoObject*;

/// The handle of no object.
inline constexpr RawFd kInvalidFd = nullptr;

/// How the lwIP Poller is run. Filled in by the Pico Runtime from its PicoNetwork.
struct PollerOptions {
    /// The SDK async_context the Poller runs (async_context_poll()) and sleeps on
    /// (async_context_wait_for_work_until()). Must outlive the Poller.
    async_context* context = nullptr;
    /// True when lwIP has been initialised and runs on `context`. Socket and DNS
    /// creation fail with ENETDOWN otherwise, and the loopback queue is not drained.
    bool lwip = false;
};
#elif defined(CORO_SYS_WINDOWS)
/// The Windows Poller's state: the completion port, AFD handles, per-socket state and
/// the timeout timer. Defined in poller_iocp.cpp, which alone includes <windows.h>.
struct IocpPoller;

/// Native handle type for a pollable I/O object: a `SOCKET`. An integer the size of a
/// pointer, so this header need not include <winsock2.h>. The file seam stores a
/// handle object of its own in it (src/detail/sys/windows/file_windows.cpp).
using RawFd = std::uintptr_t;

/// The handle of no object: all ones, which is both INVALID_SOCKET and
/// INVALID_HANDLE_VALUE.
inline constexpr RawFd kInvalidFd = ~RawFd{0};

/// Backend-specific construction options. Windows needs none.
struct PollerOptions {};
#else
/// Native handle type for a pollable I/O object: a file descriptor.
using RawFd = int;

/// The handle of no object.
inline constexpr RawFd kInvalidFd = -1;

/// Backend-specific construction options. POSIX needs none.
struct PollerOptions {};
#endif

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

/// How a registration's readiness is reported. See the backend contract above.
enum class Trigger { Edge, Level };

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
    explicit Poller(PollerOptions options = {});
    ~Poller();

    Poller(const Poller&)            = delete;
    Poller& operator=(const Poller&) = delete;
    Poller(Poller&&)                 = delete;
    Poller& operator=(Poller&&)      = delete;

    /// Starts monitoring `fd`, edge-triggered unless `trigger` says otherwise. `key` is
    /// returned in each Event for this fd and must remain valid until the fd is
    /// deregistered AND the poll() batch that might still hold it has been processed
    /// (see IoDriver's pending-release list).
    ///
    /// An error or hang-up on the fd is reported whatever `interest` is, even an
    /// empty one.
    /// @throws std::system_error on failure.
    void register_fd(RawFd fd, void* key, Interest interest, Trigger trigger = Trigger::Edge);

    /// Replaces the key, interest and trigger of an already registered fd.
    /// @throws std::system_error on failure.
    void reregister_fd(RawFd fd, void* key, Interest interest, Trigger trigger = Trigger::Edge);

    /// Stops monitoring `fd`. Must be called BEFORE the fd is closed: epoll watches the
    /// open file, not the fd number, so if the fd was closed while a dup of it is
    /// still open, the registration survives and its key can still be reported.
    /// Never throws, so it is safe to call from destructors.
    /// @return true if `fd` was registered and is now removed; false if it was not
    ///         registered, or is no longer a valid fd (closed too early, or invalid).
    bool deregister_fd(RawFd fd) noexcept;

    /// Re-enables reporting of `interest` on an edge-triggered `fd`, after its consumer
    /// saw would-block. Thread-safe. On epoll and lwIP the OS (or the backend's ready
    /// list) reports edges itself, and this does nothing. On Windows a direction that
    /// has been reported is not polled for again until this is called. Never throws:
    /// a failure to re-arm is reported as an error event on `fd`, so its waiter
    /// retries its syscall and sees the error there.
#ifdef CORO_SYS_WINDOWS
    void rearm(RawFd fd, Interest interest) noexcept;
#else
    void rearm(RawFd, Interest) noexcept {}
#endif

    /// Waits for readiness events and appends them to `out` (which is not cleared).
    ///
    /// @param timeout `std::nullopt` waits indefinitely; zero returns immediately.
    /// Returns early, possibly with no events, if interrupted by a signal.
    /// @throws std::system_error on failure other than EINTR.
    void poll(std::vector<Event>& out, std::optional<std::chrono::nanoseconds> timeout);

#ifdef CORO_SYS_LWIP
    /// Records readiness on `obj` (IoObject's kReadable, kWritable, kError, kHup
    /// bits) and queues it for the next poll() if it is registered. Called by the
    /// backend's lwIP callbacks and async_context workers, on the executor's
    /// thread. Several calls before a poll() produce one Event.
    void mark_ready(IoObject* obj, uint8_t bits) noexcept;

    /// The async_context this Poller runs on.
    async_context* context() const noexcept { return m_context; }
#endif

private:
#ifdef CORO_SYS_WINDOWS
    friend class PollWaker;
#endif
#ifdef CORO_SYS_LWIP
    // One pass of the SDK context and the loopback queue, then the teardown
    // deferred during it. See poll().
    void service();
    // True while lwIP's loopback queue holds a packet.
    bool loopback_pending() const noexcept;
    // Unlinks `obj` from the ready list.
    void unlink_ready(IoObject* obj) noexcept;

    async_context* m_context = nullptr;
    bool           m_lwip    = false;
    // FIFO of objects with readiness to report, through IoObject::next_ready.
    IoObject*      m_ready_head = nullptr;
    IoObject*      m_ready_tail = nullptr;
#elif defined(CORO_SYS_WINDOWS)
    std::unique_ptr<IocpPoller> m_impl;
#else
    RawFd m_fd = -1;  // the epoll instance
#endif
};

/**
 * @brief Wakes a thread blocked in Poller::poll() from any thread.
 *
 * Linux: an eventfd registered (readable) with the poller under a caller-chosen key.
 * lwIP: a notify object (sys/notify.h) registered the same way; wake() marks its
 * async_context worker pending, which ends a sleeping poll().
 * Windows: a packet posted to the completion port, at most one at a time.
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
#ifdef CORO_SYS_LWIP
    RawFd   m_fd = kInvalidFd;  // a notify object, registered under the waker's key
#elif defined(CORO_SYS_WINDOWS)
    void*      m_key;
    std::mutex m_mutex;
    bool       m_posted = false;  // GUARDED BY m_mutex: a wake packet is in the port
#else
    RawFd   m_fd = -1;  // eventfd
#endif
};

} // namespace coro::detail::sys
