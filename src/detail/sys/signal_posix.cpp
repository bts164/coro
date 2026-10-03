// POSIX backend for the signal seam (include/coro/detail/sys/signal.h).

#include <coro/detail/sys/signal.h>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <system_error>

namespace coro::detail::sys {

namespace {

// Written by the signal handler, so these are atomics rather than mutex-guarded: a
// handler may interrupt a thread holding any mutex, and taking it would deadlock.
// Lock-free atomics are async-signal-safe.
constexpr int kMaxSignal = NSIG;
std::array<std::atomic<uint64_t>, kMaxSignal> g_counts{};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);

// The self-pipe. Created once and never closed: a handler may run on any thread at any
// moment, so the write end must stay valid for the life of the process.
std::atomic<int> g_pipe_write{-1};
int              g_pipe_read = -1;   // set once under g_pipe_once
std::once_flag   g_pipe_once;

// Install and restore bookkeeping. Never touched by the handler.
struct Installed {
    int              refs = 0;
    struct sigaction previous {};
};
std::mutex                            g_install_mutex;
std::array<Installed, kMaxSignal>     g_installed{};   // GUARDED BY g_install_mutex

[[noreturn]] void throw_errno(int err, const char* what) {
    throw std::system_error(err, std::system_category(), what);
}

extern "C" void on_signal(int signum) {
    const int saved_errno = errno;   // the handler may interrupt code that reads errno
    // Count first, then write: a watcher reads the pipe before it reads the counts, so
    // a byte it drained always has its delivery already counted.
    g_counts[static_cast<std::size_t>(signum)].fetch_add(1);
    const int fd = g_pipe_write.load();
    if (fd >= 0) {
        const char byte = 0;
        // EAGAIN (pipe full) is fine: unread bytes already guarantee a wake-up.
        (void)!::write(fd, &byte, 1);
    }
    errno = saved_errno;
}

void ensure_pipe() {
    std::call_once(g_pipe_once, [] {
        int fds[2];
        if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0)
            throw_errno(errno, "coro::signal: pipe2");   // call_once retries next time
        g_pipe_read = fds[0];
        g_pipe_write.store(fds[1]);
    });
}

} // namespace

void signal_watch(int signum) {
    if (signum <= 0 || signum >= kMaxSignal) throw_errno(EINVAL, "coro::signal");
    ensure_pipe();

    std::lock_guard lock(g_install_mutex);
    Installed& entry = g_installed[static_cast<std::size_t>(signum)];
    if (entry.refs == 0) {
        struct sigaction action {};
        action.sa_handler = on_signal;
        sigemptyset(&action.sa_mask);
        // SA_RESTART: a blocking syscall the signal interrupts on some other thread is
        // restarted rather than failing with EINTR.
        action.sa_flags = SA_RESTART;
        if (::sigaction(signum, &action, &entry.previous) != 0)
            throw_errno(errno, "coro::signal: sigaction");   // SIGKILL, SIGSTOP: EINVAL
    }
    ++entry.refs;
}

void signal_unwatch(int signum) noexcept {
    if (signum <= 0 || signum >= kMaxSignal) return;
    std::lock_guard lock(g_install_mutex);
    Installed& entry = g_installed[static_cast<std::size_t>(signum)];
    if (entry.refs == 0) return;
    if (--entry.refs == 0) {
        // Race (benign): a delivery already inside on_signal on another thread finishes
        // normally; one arriving after this takes the restored action, which is the
        // point. Its count, if any, is seen by nobody.
        (void)::sigaction(signum, &entry.previous, nullptr);
    }
}

uint64_t signal_count(int signum) noexcept {
    if (signum <= 0 || signum >= kMaxSignal) return 0;
    return g_counts[static_cast<std::size_t>(signum)].load();
}

RawFd signal_pipe_dup() {
    ensure_pipe();
    // Each dup() shares the pipe's one open file description, so it is non-blocking
    // too, and a byte drained through any of them is gone for all. F_DUPFD_CLOEXEC
    // sets close-on-exec atomically.
    const RawFd fd = ::fcntl(g_pipe_read, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) throw_errno(errno, "coro::signal: dup");
    return fd;
}

IoResult signal_pipe_drain(RawFd fd) noexcept {
    std::size_t total = 0;
    char buf[64];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) { total += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return std::unexpected(errno);
        break;   // empty (EAGAIN), or EOF, which can't happen: the write end never closes
    }
    if (total == 0) return std::unexpected(EAGAIN);
    return total;
}

} // namespace coro::detail::sys
