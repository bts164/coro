#include <coro/detail/socket_state.h>
#include <coro/detail/sys/socket.h>
#include <coro/runtime/runtime.h>

#include <stdexcept>
#include <string>

namespace coro::detail {

SocketState::SocketState(IoDriver& driver_, sys::RawFd fd_) : driver(&driver_), fd(fd_) {
    try {
        reg = IoRegistration(driver_, fd, sys::Interest::read_write());
    } catch (...) {
        sys::close_socket(fd);   // the destructor won't run for a throwing constructor
        throw;
    }
}

SocketState::~SocketState() {
    // Deregister before close: once the fd number is closed it may be reused by a new
    // socket, and its registration must already be gone. A stale event another
    // worker's turn() already fetched still names this registration's ScheduledIo,
    // which the driver keeps alive until its next turn (see "Registration lifetime
    // and stale events" in io_driver.md).
    reg.deregister();
    sys::close_socket(fd);
}

IoDriver& socket_io_driver(const char* what) {
    Runtime& rt = current_runtime();
    if (!rt.turns_io_driver())
        throw std::logic_error(std::string(what) +
            ": this Runtime's executor doesn't turn the IoDriver (a CurrentThreadExecutor "
            "with its own Parker?); use Runtime(n)");
    return rt.io_driver();
}

} // namespace coro::detail
