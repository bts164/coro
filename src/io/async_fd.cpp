// AsyncFd. See doc/design/io_driver.md, "`AsyncFd`".

#include <coro/io/async_fd.h>

#include <coro/detail/sys/fd.h>
#include <coro/detail/sys/socket.h>

#include <cassert>

namespace coro {

AsyncFd::AsyncFd(int fd, Interest interest) {
    assert((interest.readable || interest.writable) && "AsyncFd with an empty Interest");
    try {
        IoDriver& driver = detail::external_io_driver("AsyncFd");
        // Before registering: an operation's syscall must never block the executor's
        // thread, and with it every task on it.
        detail::sys::fd_set_nonblocking(fd);
        auto state = std::make_unique<State>(fd, interest);
        state->registration = detail::make_rc<detail::NotifyRegistration>(driver, fd, interest);
        state->read.set_fixed(state->registration);
        state->write.set_fixed(state->registration);
        m_state = std::move(state);
    } catch (...) {
        // It owns the fd from the start; the destructor won't run for a throwing
        // constructor.
        detail::sys::close_socket(fd);
        throw;
    }
}

AsyncFd::~AsyncFd() {
    const int fd = take();
    if (fd != -1) detail::sys::close_socket(fd);
}

AsyncFd::AsyncFd(AsyncFd&& other) noexcept : m_state(std::move(other.m_state)) {
    assert((!m_state || (!m_state->read.has_futures() && !m_state->write.has_futures())) &&
           "AsyncFd moved while a future of it exists");
}

AsyncFd& AsyncFd::operator=(AsyncFd&& other) noexcept {
    if (this != &other) {
        assert((!other.m_state ||
                (!other.m_state->read.has_futures() && !other.m_state->write.has_futures())) &&
               "AsyncFd moved while a future of it exists");
        const int fd = take();
        if (fd != -1) detail::sys::close_socket(fd);
        m_state = std::move(other.m_state);
    }
    return *this;
}

int AsyncFd::fd() const noexcept {
    return m_state ? m_state->fd : -1;
}

int AsyncFd::release() noexcept {
    return take();
}

int AsyncFd::take() noexcept {
    if (!m_state) return -1;
    assert(!m_state->read.has_futures() && !m_state->write.has_futures() &&
           "AsyncFd destroyed or released while a future of it exists");
    // Deregister before the fd can be closed: once closed, its number may be reused
    // by a new fd, and this registration must already be gone. See io_driver.md,
    // "Registration lifetime and stale events".
    m_state->registration->reg.deregister();
    const int fd = m_state->fd;
    m_state.reset();
    return fd;
}

} // namespace coro
