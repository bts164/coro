#pragma once

// Notify seam: the driver's eventfd analog. A notify object becomes readable each
// time something outside the driver's thread signals it, which is how the ISR
// primitives (coro/sync/isr_event.h) wait on the driver. See
// doc/design/io_driver.md, "The sys layer", and doc/design/isr_safety.md, "Waiting
// on the driver".
//
// Current backends:
//   POSIX: an eventfd (src/detail/sys/posix/notify_posix.cpp)
//   lwIP:  an IoObject with its own async_context worker
//          (src/detail/sys/lwip/notify_lwip.cpp)

#include <coro/detail/sys/poller.h>

namespace coro::detail::sys {

/// Creates a notify object. Register it with the driver like any other fd, and
/// close it with close_socket() after deregistering it.
/// @throws std::system_error on failure.
RawFd notify_open();

/// Makes `fd` readable. Safe from any thread, and on the lwIP backend from an
/// interrupt handler on either core. Each signal is an edge: the next poll()
/// reports it, and nothing needs to be read back to re-arm it. Several signals
/// before a poll() may be reported as one.
///
/// On lwIP a signal made while the object is not registered is dropped (nothing can
/// be waiting); the caller's own state must record what happened.
void notify_signal(RawFd fd) noexcept;

} // namespace coro::detail::sys
