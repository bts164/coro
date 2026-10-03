#pragma once

// Backend seam for signal delivery: a process-wide self-pipe. See
// doc/design/signal_handling.md.
//
// A signal handler installed per watched signum counts each delivery and writes one
// byte to a global non-blocking pipe; that is all it does, and both are
// async-signal-safe. Each watcher reads a dup() of the pipe's read end through the
// IoDriver, and whichever one wakes first hands the new counts to every watcher.
//
// Why not signalfd: it only receives a signal that is blocked in every thread of the
// process, including threads coro doesn't own (a library's, or a user's
// std::thread started earlier). Any thread that doesn't block it takes the default
// action instead, which for SIGINT/SIGTERM kills the process. The handler needs no
// cooperation from other threads.
//
// Current backends:
//   POSIX: src/detail/sys/signal_posix.cpp

#include <coro/detail/sys/socket.h>

#include <cstdint>

namespace coro::detail::sys {

/// Starts (or keeps) counting deliveries of `signum`. The first watch of a signum
/// installs the handler with sigaction() and saves the previous action; later ones
/// only bump a reference count. Also creates the self-pipe on first use.
/// @throws std::system_error EINVAL for a signum that can't be caught (out of range,
///         SIGKILL, SIGSTOP), or on pipe/sigaction failure.
void signal_watch(int signum);

/// Undoes one signal_watch(). The last one restores the action saved by the first,
/// so e.g. SIGINT terminates the process again once nobody watches it.
void signal_unwatch(int signum) noexcept;

/// Deliveries of `signum` counted by the handler since the process started.
/// Monotonic; compare against an earlier value to get the deliveries in between.
uint64_t signal_count(int signum) noexcept;

/// A new close-on-exec, non-blocking dup() of the self-pipe's read end, for one
/// watcher to register with its IoDriver. Creates the pipe on first use.
/// @throws std::system_error on failure.
RawFd signal_pipe_dup();

/// Reads everything currently in the self-pipe through `fd`. Returns the byte count if
/// it read any, EAGAIN if the pipe was already empty (another watcher drained it).
IoResult signal_pipe_drain(RawFd fd) noexcept;

} // namespace coro::detail::sys
