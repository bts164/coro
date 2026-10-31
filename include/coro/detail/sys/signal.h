#pragma once

// Backend seam for signal delivery. See doc/design/signal_handling.md.
//
// A handler counts each delivery of a watched signum and wakes every watcher, each of
// which reads its own "pipe" fd through the IoDriver; whichever wakes first hands the
// new counts to every watcher.
//
// POSIX: a sigaction() handler per watched signum counts the delivery and writes one
// byte to a global non-blocking self-pipe; both are async-signal-safe. Each watcher's
// fd is a dup() of the pipe's read end, so a byte drained through any is gone for all.
//
// Why not signalfd: it only receives a signal that is blocked in every thread of the
// process, including threads coro doesn't own (a library's, or a user's
// std::thread started earlier). Any thread that doesn't block it takes the default
// action instead, which for SIGINT/SIGTERM kills the process. The handler needs no
// cooperation from other threads.
//
// Windows: the signums are console control events (CTRL_C_EVENT, ...), and the handler
// is a SetConsoleCtrlHandler() routine, which Windows runs on a thread of its own. Each
// watcher's fd is a loopback UDP socket of its own, and the handler sends one datagram
// to every one, so the IoDriver's AFD poller sees an ordinary socket.
//
// Current backends:
//   POSIX:   src/detail/sys/posix/signal_posix.cpp
//   Windows: src/detail/sys/windows/signal_windows.cpp

#include <coro/detail/sys/socket.h>

#include <cstdint>

namespace coro::detail::sys {

/// Starts (or keeps) counting deliveries of `signum`. The first watch of a signum
/// installs the handler (POSIX: sigaction(), saving the previous action) and later
/// ones only bump a reference count. Also creates the self-pipe on first use.
/// @throws std::system_error EINVAL for a signum that can't be caught (out of range,
///         SIGKILL, SIGSTOP; on Windows, not a console control event), or on failure to
///         install the handler.
void signal_watch(int signum);

/// Undoes one signal_watch(). The last one restores the default behaviour, so e.g.
/// SIGINT or Ctrl-C terminates the process again once nobody watches it.
void signal_unwatch(int signum) noexcept;

/// Deliveries of `signum` counted by the handler since the process started.
/// Monotonic; compare against an earlier value to get the deliveries in between.
uint64_t signal_count(int signum) noexcept;

/// A new non-blocking, non-inheritable fd that becomes readable on each delivery, for
/// one watcher to register with its IoDriver: a dup() of the self-pipe's read end on
/// POSIX, a loopback UDP socket on Windows. Close it with close_socket(), after
/// signal_pipe_release().
/// @throws std::system_error on failure.
RawFd signal_pipe_dup();

/// Stops deliveries being sent to `fd`, before it is closed. Windows only; a no-op on
/// POSIX, where the pipe is shared and nothing writes to a particular dup.
void signal_pipe_release(RawFd fd) noexcept;

/// Reads everything currently readable through `fd`. Returns the byte count if it read
/// any, EAGAIN if there was nothing (on POSIX, another watcher may have drained it).
IoResult signal_pipe_drain(RawFd fd) noexcept;

#ifndef CORO_SYS_WINDOWS
/// True if `signum` is set to SIG_IGN and nobody watches it, e.g. SIGHUP under nohup.
/// For shutdown_requested(), which leaves such a signal alone rather than undo the
/// ignore. POSIX only.
bool signal_ignored(int signum) noexcept;
#endif

#ifdef CORO_SYS_WINDOWS
/// Runs the console control handler for `event` on the calling thread, as Windows runs
/// it on its own thread, and returns what the handler returned (false: not watched, so
/// Windows would go on to the next handler and the default, ExitProcess()). For close,
/// logoff and shutdown events it doesn't return until every watcher of the event has
/// been dropped.
///
/// For tests: Windows can only send a console event to every process attached to the
/// console, the test runner included.
bool signal_console_event(unsigned long event);
#endif

} // namespace coro::detail::sys
