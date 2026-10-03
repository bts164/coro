// epoll backend for coro::detail::sys::Poller / PollWaker.
// See include/coro/detail/sys/poller.h for the backend contract.

#include <coro/detail/sys/poller.h>

#ifndef __linux__
#error "poller_epoll.cpp is the Linux backend; this platform needs its own sys::Poller"
#endif

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <ctime>
#include <system_error>

#include <csignal>
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

// epoll_pwait2() (Linux 5.11+) takes a timespec, giving sub-millisecond timeouts.
// It is called through syscall() so coro doesn't depend on a glibc new enough to
// wrap it (2.35+). Older kernels return ENOSYS; we then fall back to epoll_wait()
// with the timeout rounded UP to whole milliseconds (never wake early).
//
// One-way capability flag: only ever flips true → false, and a stale `true` read
// just costs one more ENOSYS. A mutex would buy nothing here, hence the atomic.
std::atomic<bool> g_have_epoll_pwait2{true};

int wait_ms(int epfd, epoll_event* events, std::chrono::nanoseconds timeout) {
    using namespace std::chrono;
    const auto ms = ceil<milliseconds>(timeout).count();
    const int clamped = ms > INT_MAX ? INT_MAX : static_cast<int>(ms);
    return ::epoll_wait(epfd, events, kMaxEventsPerPoll, clamped);
}

int wait_with_timeout(int epfd, epoll_event* events,
                      std::optional<std::chrono::nanoseconds> timeout) {
    if (!timeout)
        return ::epoll_wait(epfd, events, kMaxEventsPerPoll, -1);
    if (timeout->count() <= 0)
        return ::epoll_wait(epfd, events, kMaxEventsPerPoll, 0);

#ifdef SYS_epoll_pwait2
    if (g_have_epoll_pwait2.load(std::memory_order_relaxed)) {
        const auto ns = timeout->count();
        timespec ts{};
        ts.tv_sec  = static_cast<time_t>(ns / 1'000'000'000);
        ts.tv_nsec = static_cast<long>(ns % 1'000'000'000);
        // sigmask == nullptr: the kernel ignores sigsetsize, but pass the real size.
        const long rc = ::syscall(SYS_epoll_pwait2, epfd, events, kMaxEventsPerPoll,
                                  &ts, nullptr, _NSIG / 8);
        if (rc >= 0 || errno != ENOSYS)
            return static_cast<int>(rc);
        g_have_epoll_pwait2.store(false, std::memory_order_relaxed);
    }
#endif
    return wait_ms(epfd, events, *timeout);
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
