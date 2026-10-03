#pragma once

// Desktop only: the state every IoDriver-backed fd (UdpSocket, TcpStream, TcpListener,
// and Pipe, whose FIFO fd needs exactly the same handling) shares with its in-flight
// futures. See doc/design/udp_socket.md, "Desktop (IoDriver) backend", and
// doc/design/io_driver.md, "Registration lifetime and stale events".

#include <coro/detail/sys/poller.h>
#include <coro/runtime/io_driver.h>

#include <exception>
#include <system_error>

namespace coro::detail {

/**
 * @brief A socket's fd and its IoDriver registration (both directions).
 *
 * Shared through a `shared_ptr` by the socket object and every future it hands out,
 * so the fd is closed only when the last owner drops it: never under a syscall
 * running on another worker.
 */
struct SocketState {
    /// Registers `fd`; takes ownership of it, closing it if registration throws.
    SocketState(IoDriver& driver, sys::RawFd fd);
    /// Deregisters, then closes the fd. Runs on whichever thread drops the last owner.
    ~SocketState();

    SocketState(const SocketState&)            = delete;
    SocketState& operator=(const SocketState&) = delete;

    /// The driver `reg` belongs to. TcpListener registers accepted sockets with it.
    IoDriver*      driver;
    sys::RawFd     fd;
    IoRegistration reg;
};

/**
 * @brief The current Runtime's IoDriver, for a socket being created.
 *
 * @throws std::logic_error naming `what` if the Runtime's executor never turns the
 *         driver: a socket there could never be woken, so it fails loudly up front
 *         instead of hanging at its first wait.
 */
IoDriver& socket_io_driver(const char* what);

/// The exception a socket future fails with for errno `err`.
inline std::exception_ptr socket_error(int err, const char* what) {
    return std::make_exception_ptr(std::system_error(err, std::system_category(), what));
}

} // namespace coro::detail
