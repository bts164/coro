#pragma once

// Test helpers for driving real sockets through the Runtime's IoDriver.
// Used by test_current_thread_executor.cpp, test_work_stealing_io.cpp and
// test_task_budget.cpp. The sockets themselves come from socket_test_util.h.

#include <coro/runtime/io_driver.h>
#include <coro/runtime/runtime.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/sys/poller.h>

#include <gtest/gtest.h>

#include "socket_test_util.h"

#include <exception>
#include <stdexcept>

namespace coro::io_test {

using detail::sys::RawFd;
using test_sockets::SocketPair;
using test_sockets::write_byte;

/// One readiness-handshake read of a single byte. The registration is made lazily
/// on the first poll, from current_runtime(), so the future can be built off-runtime.
inline PollResult<char> poll_read_byte(RawFd fd, IoRegistration& reg, detail::Context& ctx) {
    if (!reg)
        reg = IoRegistration(current_runtime().io_driver(), fd, detail::sys::Interest::read());
    for (;;) {
        auto ready = reg.poll_ready(IoDirection::Read, ctx);
        if (!ready) return PollPending;
        char c;
        auto n = test_sockets::read_some(fd, &c, 1);
        if (n && *n == 1) return c;
        if (!n && detail::sys::would_block(n.error())) {
            reg.clear_ready(*ready);
            continue;
        }
        return PollError(std::make_exception_ptr(std::runtime_error("read failed")));
    }
}

/// Reads one byte from a non-blocking socket; owns its registration (one-shot).
struct ReadByteFuture {
    using OutputType = char;
    RawFd          m_fd;
    IoRegistration m_reg;

    PollResult<char> poll(detail::Context& ctx) { return poll_read_byte(m_fd, m_reg, ctx); }
};

/// Reads bytes from one socket repeatedly through a single registration, so a loop of
/// reads doesn't re-register it every time. Must outlive the futures it returns.
class AsyncByteReader {
public:
    explicit AsyncByteReader(RawFd fd) : m_fd(fd) {}

    struct ReadFuture {
        using OutputType = char;
        AsyncByteReader* m_reader;
        PollResult<char> poll(detail::Context& ctx) {
            return poll_read_byte(m_reader->m_fd, m_reader->m_reg, ctx);
        }
    };

    [[nodiscard]] ReadFuture read() { return ReadFuture{this}; }

private:
    RawFd          m_fd;
    IoRegistration m_reg;
};

} // namespace coro::io_test
