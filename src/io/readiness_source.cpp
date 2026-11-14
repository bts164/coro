// ReadinessSource, and the per-direction state it shares with AsyncFd. See
// doc/design/io_driver.md, "External drivers".

#include <coro/io/readiness_source.h>

#include <coro/detail/socket_state.h>
#include <coro/detail/sys/notify.h>
#include <coro/detail/sys/socket.h>
#include <coro/runtime/runtime.h>

#include <cassert>
#include <mutex>
#include <stdexcept>
#include <string>

namespace coro {

namespace detail {

IoDriver& external_io_driver(const char* what) {
    // current_runtime() reports "no runtime" as a runtime_error; for these types it
    // is a usage error, like a runtime whose executor never turns the driver.
    try {
        (void)current_runtime();
    } catch (const std::runtime_error&) {
        throw std::logic_error(std::string(what) + ": called outside a coro::Runtime");
    }
    return socket_io_driver(what);
}

ReadinessDirection::~ReadinessDirection() {
    assert(!has_futures() && "AsyncFd or ReadinessSource destroyed while a future of it exists");
    assert(!registered_lazily() && "ReadinessSource destroyed while registered");
}

void ReadinessDirection::set_fixed(Rc<NotifyRegistration> registration) noexcept {
    assert(!has_futures());
    m_fixed = std::move(registration);
}

Rc<NotifyRegistration> ReadinessDirection::acquire() {
    if (m_fixed) return m_fixed;
    // Race (handled): two first waiters on different executor threads both find no
    // registration. Under the lock, the second finds the first one's, instead of
    // registering the same fd again (which epoll refuses with EEXIST).
    std::lock_guard lock(m_mutex);
    if (auto existing = m_lazy.lock()) return existing;
    auto created = make_rc<NotifyRegistration>(external_io_driver(m_what), m_fd);
    m_lazy = Weak<NotifyRegistration>(created);
    return created;
}

void ReadinessDirection::release(IsrWaitCore& core) noexcept {
    // Race (handled): the last share's drop deregisters the fd. Under the lock
    // acquire() takes, so a new first waiter cannot register the fd while it is
    // still registered: it finds the old registration either alive (and shares it)
    // or fully gone.
    std::lock_guard lock(m_mutex);
    core.reset();
}

bool ReadinessDirection::registered() const {
    if (m_fixed) return true;
    return registered_lazily();
}

bool ReadinessDirection::registered_lazily() const {
    std::lock_guard lock(m_mutex);
    return !m_lazy.expired();
}

void ReadinessDirection::future_attached() noexcept {
#ifndef NDEBUG
    std::lock_guard lock(m_mutex);
    ++m_futures;
#endif
}

void ReadinessDirection::future_detached() noexcept {
#ifndef NDEBUG
    std::lock_guard lock(m_mutex);
    assert(m_futures > 0);
    --m_futures;
#endif
}

bool ReadinessDirection::has_futures() const noexcept {
#ifndef NDEBUG
    std::lock_guard lock(m_mutex);
    return m_futures != 0;
#else
    return false;
#endif
}

} // namespace detail

ReadinessSource::ReadinessSource() {
    m_read_fd = detail::sys::notify_open();
    try {
        m_write_fd = detail::sys::notify_open();
    } catch (...) {
        detail::sys::close_socket(m_read_fd);
        throw;
    }
    m_read.set_notify_fd(m_read_fd);
    m_write.set_notify_fd(m_write_fd);
}

ReadinessSource::~ReadinessSource() {
    // No future exists (a precondition, which m_read/m_write assert), so neither fd
    // is registered: the last waiter in each direction deregistered it.
    assert(!m_read.has_futures() && !m_write.has_futures() &&
           "ReadinessSource destroyed while a future of it exists");
    detail::sys::close_socket(m_read_fd);
    detail::sys::close_socket(m_write_fd);
}

void ReadinessSource::signal_readable() noexcept {
    detail::sys::notify_signal(m_read_fd);
}

void ReadinessSource::signal_writable() noexcept {
    detail::sys::notify_signal(m_write_fd);
}

void ReadinessSource::signal() noexcept {
    detail::sys::notify_signal(m_read_fd);
    detail::sys::notify_signal(m_write_fd);
}

} // namespace coro
