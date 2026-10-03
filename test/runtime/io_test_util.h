#pragma once

// Test helpers for driving real fds through the Runtime's IoDriver.
// Used by test_current_thread_executor.cpp and test_work_stealing_io.cpp.

#include <coro/runtime/io_driver.h>
#include <coro/runtime/runtime.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <exception>
#include <stdexcept>

namespace coro::io_test {

/// A connected, non-blocking AF_UNIX stream socketpair; closes both ends.
struct SocketPair {
    int a = -1;
    int b = -1;
    SocketPair() {
        int fds[2];
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
        a = fds[0];
        b = fds[1];
    }
    ~SocketPair() {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
    }
    SocketPair(const SocketPair&)            = delete;
    SocketPair& operator=(const SocketPair&) = delete;
};

/// Writes one byte; a 1-byte write to a socketpair never hits a full buffer here.
inline void write_byte(int fd, char c = 'x') {
    EXPECT_EQ(::write(fd, &c, 1), 1);
}

/// One readiness-handshake read of a single byte. The registration is made lazily
/// on the first poll, from current_runtime(), so the future can be built off-runtime.
inline PollResult<char> poll_read_byte(int fd, IoRegistration& reg, detail::Context& ctx) {
    if (!reg)
        reg = IoRegistration(current_runtime().io_driver(), fd, detail::sys::Interest::read());
    for (;;) {
        auto ready = reg.poll_ready(IoDirection::Read, ctx);
        if (!ready) return PollPending;
        char c;
        const ssize_t n = ::read(fd, &c, 1);
        if (n == 1) return c;
        if (n < 0 && errno == EAGAIN) {
            reg.clear_ready(*ready);
            continue;
        }
        return PollError(std::make_exception_ptr(std::runtime_error("read failed")));
    }
}

/// Reads one byte from a non-blocking fd; owns its registration (one-shot).
struct ReadByteFuture {
    using OutputType = char;
    int            m_fd;
    IoRegistration m_reg;

    PollResult<char> poll(detail::Context& ctx) { return poll_read_byte(m_fd, m_reg, ctx); }
};

/// Reads bytes from one fd repeatedly through a single registration, so a loop of
/// reads doesn't re-register the fd every time. Must outlive the futures it returns.
class AsyncByteReader {
public:
    explicit AsyncByteReader(int fd) : m_fd(fd) {}

    struct ReadFuture {
        using OutputType = char;
        AsyncByteReader* m_reader;
        PollResult<char> poll(detail::Context& ctx) {
            return poll_read_byte(m_reader->m_fd, m_reader->m_reg, ctx);
        }
    };

    [[nodiscard]] ReadFuture read() { return ReadFuture{this}; }

private:
    int            m_fd;
    IoRegistration m_reg;
};

} // namespace coro::io_test
