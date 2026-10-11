// epoll backend for coro::detail::sys::Poller / PollWaker.
// See include/coro/detail/sys/poller.h for the backend contract.

#include <coro/detail/sys/poller.h>

#ifndef __linux__
#error "poller_epoll.cpp is the Linux backend; this platform needs its own sys::Poller"
#endif

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <system_error>

#include <csignal>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace coro::detail::sys {

namespace {

// Events fetched per epoll_wait() call. More than this many ready fds are simply
// returned by the next poll(); with edge triggering nothing is lost.
constexpr int kMaxEventsPerPoll = 256;

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

uint32_t to_epoll_events(Interest interest) {
    uint32_t ev = EPOLLET;
    if (interest.readable) ev |= EPOLLIN | EPOLLRDHUP;
    if (interest.writable) ev |= EPOLLOUT;
    return ev;
}

// A timed wait, with the timeout at nanosecond resolution. epoll_wait() takes its
// timeout in whole milliseconds, but an epoll instance is itself pollable (readable
// while it has events to report) and ppoll() takes a timespec. So: wait in ppoll()
// on the epoll fd, then collect the events without blocking. ppoll() is on every
// kernel coro runs on, so this is the one path, and every platform's tests run it.
//
// Only a thread about to sleep until a timer gets here; see wait_with_timeout().
//
// Nothing is lost between the two calls: Poller::poll() is called by one thread at a
// time, so no one else can take the events ppoll() saw.
int wait_ppoll(int epfd, epoll_event* events, std::chrono::nanoseconds timeout) {
    const auto ns = timeout.count();
    timespec ts{};
    ts.tv_sec  = static_cast<time_t>(ns / 1'000'000'000);
    ts.tv_nsec = static_cast<long>(ns % 1'000'000'000);
    pollfd pfd{};
    pfd.fd     = epfd;
    pfd.events = POLLIN;
    const int rc = ::ppoll(&pfd, 1, &ts, nullptr);
    if (rc <= 0) return rc;  // 0: timed out. < 0: errno is set, as for epoll_wait().
    return ::epoll_wait(epfd, events, kMaxEventsPerPoll, 0);
}

// Not compiled: nothing defines CORO_USE_EPOLL_PWAIT2. epoll_pwait2() (Linux 5.11+)
// does the timed wait in one system call where wait_ppoll() takes two when events
// arrive. It is kept in case profiling shows that second call to cost something;
// consider enabling it again then. It was set aside because it cannot be the only
// path (Rocky Linux 8's kernel does not have it), and the path taken without it
// would then run only on kernels CI does not have.
#ifdef CORO_USE_EPOLL_PWAIT2

// Called through syscall() so coro doesn't depend on a glibc new enough to wrap it
// (2.35+). Headers older than the call (Rocky Linux 8's) do not name its number
// either, though the kernel the program then runs on may have it. The number is
// kernel ABI: 441 on the architectures listed, which share the generic syscall
// numbering here.
#if !defined(SYS_epoll_pwait2) && (defined(__x86_64__) || defined(__aarch64__))
#define SYS_epoll_pwait2 441
#endif

// One-way capability flag: only ever flips true → false, and a stale `true` read
// just costs one more ENOSYS. A mutex would buy nothing here, hence the atomic.
std::atomic<bool> g_have_epoll_pwait2{true};

// Returns false, with nothing waited for, when the kernel lacks the call (ENOSYS);
// the caller then uses wait_ppoll(). Otherwise sets `result` as epoll_wait() would.
bool try_epoll_pwait2(int epfd, epoll_event* events, std::chrono::nanoseconds timeout,
                      int& result) {
#ifdef SYS_epoll_pwait2
    if (g_have_epoll_pwait2.load(std::memory_order_relaxed)) {
        const auto ns = timeout.count();
        timespec ts{};
        ts.tv_sec  = static_cast<time_t>(ns / 1'000'000'000);
        ts.tv_nsec = static_cast<long>(ns % 1'000'000'000);
        // sigmask == nullptr: the kernel ignores sigsetsize, but pass the real size.
        const long rc = ::syscall(SYS_epoll_pwait2, epfd, events, kMaxEventsPerPoll,
                                  &ts, nullptr, _NSIG / 8);
        if (rc >= 0 || errno != ENOSYS) {
            result = static_cast<int>(rc);
            return true;
        }
        g_have_epoll_pwait2.store(false, std::memory_order_relaxed);
    }
#endif
    return false;
}

#endif  // CORO_USE_EPOLL_PWAIT2

// The three waits a caller can ask for. Only the last takes the two-call path:
//
//   no timeout    Block until an event. One epoll_wait(). A thread with nothing to
//                 run and no timer pending.
//   zero or less  Collect what is ready and return at once. One epoll_wait(). A
//                 thread that still has tasks to run and is only checking for I/O
//                 between them, or one whose earliest timer is already due.
//   positive      Block until an event or the timeout. wait_ppoll(). A thread with
//                 nothing to run, about to sleep until its earliest timer.
//
// So a busy thread never pays for the second system call: it is the cost of waking
// from a sleep that a timer bounded, and only when an event ended that sleep early.
int wait_with_timeout(int epfd, epoll_event* events,
                      std::optional<std::chrono::nanoseconds> timeout) {
    if (!timeout)
        return ::epoll_wait(epfd, events, kMaxEventsPerPoll, -1);
    if (timeout->count() <= 0)
        return ::epoll_wait(epfd, events, kMaxEventsPerPoll, 0);

#ifdef CORO_USE_EPOLL_PWAIT2
    int result = 0;
    if (try_epoll_pwait2(epfd, events, *timeout, result))
        return result;
#endif
    return wait_ppoll(epfd, events, *timeout);
}

} // namespace

// ---------------------------------------------------------------------------
// Poller
// ---------------------------------------------------------------------------

Poller::Poller() {
    m_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (m_fd < 0) throw_errno("epoll_create1");
}

Poller::~Poller() {
    if (m_fd >= 0) ::close(m_fd);
}

void Poller::register_fd(RawFd fd, void* key, Interest interest) {
    epoll_event ev{};
    ev.events   = to_epoll_events(interest);
    ev.data.ptr = key;
    if (::epoll_ctl(m_fd, EPOLL_CTL_ADD, fd, &ev) < 0) throw_errno("epoll_ctl(ADD)");
}

void Poller::reregister_fd(RawFd fd, void* key, Interest interest) {
    epoll_event ev{};
    ev.events   = to_epoll_events(interest);
    ev.data.ptr = key;
    if (::epoll_ctl(m_fd, EPOLL_CTL_MOD, fd, &ev) < 0) throw_errno("epoll_ctl(MOD)");
}

bool Poller::deregister_fd(RawFd fd) noexcept {
    // EPOLL_CTL_DEL also drops any of this fd's events still sitting on the ready
    // list, so no poll() that starts after this returns can report it. A poll()
    // that already copied an event out (on another thread) may still report it —
    // which is why IoDriver defers freeing the key (see IoDriver::deregister()).
    //
    // An error (EBADF, ENOENT) means this fd number isn't registered. That does NOT
    // mean nothing is monitored: if the fd was closed while a dup stayed open, the
    // old registration lives on and can't be removed through this number any more.
    // Reported to the caller, which decides whether that is a bug.
    epoll_event ev{};  // non-null for pre-2.6.9 kernels; contents ignored
    return ::epoll_ctl(m_fd, EPOLL_CTL_DEL, fd, &ev) == 0;
}

void Poller::poll(std::vector<Event>& out, std::optional<std::chrono::nanoseconds> timeout) {
    epoll_event events[kMaxEventsPerPoll];
    const int n = wait_with_timeout(m_fd, events, timeout);
    if (n < 0) {
        if (errno == EINTR) return;  // spurious return; callers re-check their state
        throw_errno("epoll_wait");
    }
    out.reserve(out.size() + static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const uint32_t e = events[i].events;
        Event ev;
        ev.key      = events[i].data.ptr;
        ev.readable = (e & (EPOLLIN | EPOLLPRI)) != 0;
        ev.writable = (e & EPOLLOUT) != 0;
        ev.error    = (e & EPOLLERR) != 0;
        ev.hup      = (e & (EPOLLHUP | EPOLLRDHUP)) != 0;
        out.push_back(ev);
    }
}

// ---------------------------------------------------------------------------
// PollWaker
// ---------------------------------------------------------------------------

PollWaker::PollWaker(Poller& poller, void* key) : m_poller(poller) {
    m_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_fd < 0) throw_errno("eventfd");
    try {
        m_poller.register_fd(m_fd, key, Interest::read());
    } catch (...) {
        ::close(m_fd);
        throw;
    }
}

PollWaker::~PollWaker() {
    (void)m_poller.deregister_fd(m_fd);  // registered in the constructor; can't fail
    ::close(m_fd);
}

void PollWaker::wake() noexcept {
    // Every eventfd write produces a new epoll edge, so concurrent wakes from several
    // threads each succeed and none is lost. EAGAIN means the counter is saturated
    // (~2^64 unconsumed wakes) — a wake is certainly pending, so it is ignored.
    const uint64_t one = 1;
    ssize_t rc;
    do {
        rc = ::write(m_fd, &one, sizeof(one));
    } while (rc < 0 && errno == EINTR);
}

void PollWaker::reset() noexcept {
    // Zero the counter. Not strictly required with EPOLLET (each write is an edge),
    // but it keeps the counter from ever saturating.
    //
    // Race: a wake() landing between poll() returning and this read() is consumed
    // here. That's harmless — the caller is the polling thread, already awake, and
    // re-checks its queues before it polls again.
    uint64_t value;
    ssize_t rc;
    do {
        rc = ::read(m_fd, &value, sizeof(value));
    } while (rc < 0 && errno == EINTR);
}

} // namespace coro::detail::sys
