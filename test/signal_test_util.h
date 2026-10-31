#pragma once

// Watching and raising a signal from a test, the same way on every platform.
//
//   POSIX:   SIGUSR1 and SIGUSR2, watched with coro::signal() and raised with ::raise(),
//            which runs the handler on the calling thread before it returns.
//   Windows: Ctrl-Break and Ctrl-C, watched with coro::windows::console_event() and
//            raised with sys::signal_console_event(), which runs the console handler on
//            the calling thread as Windows would on its own. Windows itself can only send
//            a console event to every process on the console, the test runner included.
//
// Tests run one at a time, so these have no watchers outside the test body.

#include <coro/io/signal.h>

#include <initializer_list>

#ifdef _WIN32
#include <coro/detail/sys/signal.h>
#else
#include <csignal>
#endif

namespace signal_test {

#ifdef _WIN32

using Signal = coro::windows::ConsoleEvent;
inline constexpr Signal kSignalA = Signal::CtrlBreak;
inline constexpr Signal kSignalB = Signal::CtrlC;

inline coro::SignalFuture watch(Signal s) { return coro::windows::console_event(s); }
inline coro::SignalStream watch_stream(std::initializer_list<Signal> s) {
    return coro::windows::console_event_stream(s);
}
/// Runs the handler for `s`; true if a watcher took it (false: the default would run).
inline bool raise(Signal s) {
    return coro::detail::sys::signal_console_event(static_cast<unsigned long>(s));
}
inline int signum(Signal s) { return static_cast<int>(s); }

#else

using Signal = int;
inline constexpr Signal kSignalA = SIGUSR1;
inline constexpr Signal kSignalB = SIGUSR2;

inline coro::SignalFuture watch(Signal s) { return coro::signal(s); }
inline coro::SignalStream watch_stream(std::initializer_list<Signal> s) {
    return coro::signal_stream(s);
}
inline bool raise(Signal s) { return ::raise(s) == 0; }
inline int signum(Signal s) { return s; }

#endif

} // namespace signal_test
