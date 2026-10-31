// Windows backend for the signal seam (include/coro/detail/sys/signal.h): console
// control events. See doc/design/signal_handling.md, "Windows".

#include "win.h"

#include <coro/detail/sys/signal.h>
#include <coro/io/signal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace coro::detail::sys {

namespace {

// The public enum's values are passed through as signums.
using coro::windows::ConsoleEvent;
static_assert(static_cast<int>(ConsoleEvent::CtrlC) == CTRL_C_EVENT);
static_assert(static_cast<int>(ConsoleEvent::CtrlBreak) == CTRL_BREAK_EVENT);
static_assert(static_cast<int>(ConsoleEvent::Close) == CTRL_CLOSE_EVENT);
static_assert(static_cast<int>(ConsoleEvent::Logoff) == CTRL_LOGOFF_EVENT);
static_assert(static_cast<int>(ConsoleEvent::Shutdown) == CTRL_SHUTDOWN_EVENT);

// CTRL_SHUTDOWN_EVENT (6) is the highest console control event.
constexpr int kMaxEvent = CTRL_SHUTDOWN_EVENT + 1;

bool is_console_event(int event) noexcept {
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            return true;
        default:
            return false;
    }
}

// Windows ends the process as soon as the handler returns from one of these, so the
// handler waits for the application first. See on_console_event().
bool ends_process(int event) noexcept {
    return event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT ||
           event == CTRL_SHUTDOWN_EVENT;
}

// One watcher's socket and the address it receives on.
struct Receiver {
    SOCKET      socket;
    sockaddr_in address;
};

struct State {
    std::array<std::atomic<uint64_t>, kMaxEvent> counts{};   // read without the mutex

    std::mutex              mutex;
    std::condition_variable unwatched;   // a refs entry dropped to 0
    std::array<int, kMaxEvent> refs{};   // GUARDED BY mutex
    bool                    installed = false;        // GUARDED BY mutex
    SOCKET                  sender    = INVALID_SOCKET;   // GUARDED BY mutex
    std::vector<Receiver>   receivers;   // GUARDED BY mutex
};

State& state() {
    // Leaked on purpose: the handler thread may be waiting on `unwatched` while the
    // process exits, and must not find the mutex destroyed under it.
    static State* s = new State;
    return *s;
}

SOCKET open_udp_socket() {
    const SOCKET s = ::WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                                  WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) win::throw_errno(win::last_wsa_errno(), "coro::signal: socket");
    u_long nonblocking = 1;
    if (::ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
        const int err = win::last_wsa_errno();
        ::closesocket(s);
        win::throw_errno(err, "coro::signal: ioctlsocket");
    }
    return s;
}

// The handler. Windows runs it on a thread it creates for the event, an ordinary
// thread: unlike a POSIX signal handler it may take locks and make any call.
BOOL WINAPI on_console_event(DWORD type) {
    const int event = static_cast<int>(type);
    if (!is_console_event(event)) return FALSE;
    State& s = state();
    std::unique_lock lock(s.mutex);
    // Nobody watches it: go on to the next handler, and in the end to the default,
    // ExitProcess(). That is how the last unwatch restores the default behaviour
    // without removing the handler, which could race with a call to it.
    if (s.refs[event] == 0) return FALSE;

    // Count first, then send: a watcher drains its socket before it reads the counts,
    // so a datagram it drained always has its delivery already counted. A datagram that
    // doesn't fit in a receiver's buffer is dropped, which is fine: the unread ones
    // already guarantee a wake-up.
    s.counts[static_cast<std::size_t>(event)].fetch_add(1);
    const char byte = 0;
    for (const Receiver& r : s.receivers)
        (void)::sendto(s.sender, &byte, 1, 0, reinterpret_cast<const sockaddr*>(&r.address),
                       sizeof(r.address));

    // Close, logoff, shutdown: the process ends when this returns, so wait for the
    // application to finish with it, which it says by dropping its last watcher of the
    // event. Windows kills the process anyway once its own timeout expires.
    if (ends_process(event)) s.unwatched.wait(lock, [&] { return s.refs[event] == 0; });
    return TRUE;
}

} // namespace

void signal_watch(int signum) {
    if (!is_console_event(signum)) win::throw_errno(EINVAL, "coro::signal");
    State& s = state();
    std::lock_guard lock(s.mutex);
    // Installed once and never removed (see on_console_event()). A handler added later
    // runs before this one; one added earlier runs after it, when this one returns
    // FALSE, so the application's own handlers are chained, not replaced.
    if (!s.installed) {
        if (!::SetConsoleCtrlHandler(&on_console_event, TRUE))
            win::throw_errno(win::win32_to_errno(::GetLastError()),
                             "coro::signal: SetConsoleCtrlHandler");
        s.installed = true;
    }
    ++s.refs[static_cast<std::size_t>(signum)];
}

void signal_unwatch(int signum) noexcept {
    if (!is_console_event(signum)) return;
    State& s = state();
    {
        std::lock_guard lock(s.mutex);
        int& refs = s.refs[static_cast<std::size_t>(signum)];
        if (refs == 0) return;
        if (--refs != 0) return;
    }
    // A handler waiting for this event's watchers to go may now return, and the
    // process end.
    s.unwatched.notify_all();
}

uint64_t signal_count(int signum) noexcept {
    if (!is_console_event(signum)) return 0;
    return state().counts[static_cast<std::size_t>(signum)].load();
}

RawFd signal_pipe_dup() {
    win::winsock_init();
    State& s = state();
    const SOCKET receiver = open_udp_socket();
    try {
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        if (::bind(receiver, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            win::throw_errno(win::last_wsa_errno(), "coro::signal: bind");
        int length = sizeof(address);
        if (::getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            win::throw_errno(win::last_wsa_errno(), "coro::signal: getsockname");

        std::lock_guard lock(s.mutex);
        // The sender is shared by every receiver and never closed: the handler may be
        // sending at any moment.
        if (s.sender == INVALID_SOCKET) s.sender = open_udp_socket();
        s.receivers.push_back(Receiver{receiver, address});
    } catch (...) {
        ::closesocket(receiver);
        throw;
    }
    return win::to_fd(receiver);
}

void signal_pipe_release(RawFd fd) noexcept {
    State& s = state();
    std::lock_guard lock(s.mutex);
    // Under the mutex the handler sends under: once this returns, nothing more is sent
    // to the socket's port, which another socket may be given once this one is closed.
    std::erase_if(s.receivers, [&](const Receiver& r) { return r.socket == win::to_socket(fd); });
}

IoResult signal_pipe_drain(RawFd fd) noexcept {
    std::size_t total = 0;
    char buf[64];
    for (;;) {
        // One datagram per call, each a single byte.
        const int n = ::recv(win::to_socket(fd), buf, sizeof(buf), 0);
        if (n > 0) { total += static_cast<std::size_t>(n); continue; }
        if (n == 0) continue;   // an empty datagram; none are sent, but skip it
        const int err = ::WSAGetLastError();
        if (err == WSAEMSGSIZE) { ++total; continue; }   // can't happen: 1-byte datagrams
        if (err == WSAEWOULDBLOCK) break;
        return std::unexpected(win::wsa_to_errno(err));
    }
    if (total == 0) return std::unexpected(EAGAIN);
    return total;
}

bool signal_console_event(unsigned long event) {
    return on_console_event(static_cast<DWORD>(event)) != FALSE;
}

} // namespace coro::detail::sys
