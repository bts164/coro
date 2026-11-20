// AsyncFd: a file descriptor that the caller reads and writes itself, with the
// waiting done by the runtime's IoDriver. POSIX only. See doc/design/io_driver.md,
// "External drivers" and "`AsyncFd`".
//
// The operations here make their syscalls directly, as a driver outside coro would.

#include <gtest/gtest.h>
#include <coro/coro.h>
#include <coro/io/async_fd.h>
#include <coro/runtime/runtime.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/task/join_handle.h>
#include "executor_traits.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <string>
#include <system_error>

using namespace coro;
using namespace std::chrono_literals;

namespace {

static_assert(Future<ReadinessFuture<decltype([] { return std::expected<std::size_t, int>(0); })>>);

using IoResult = std::expected<std::size_t, int>;

IoResult read_some(int fd, std::byte* data, std::size_t size) {
    const ssize_t n = ::read(fd, data, size);
    if (n < 0) return std::unexpected(errno);
    return static_cast<std::size_t>(n);
}

IoResult send_some(int fd, const std::byte* data, std::size_t size) {
    // MSG_NOSIGNAL: a send after the peer closed fails with EPIPE, without SIGPIPE.
    const ssize_t n = ::send(fd, data, size, MSG_NOSIGNAL);
    if (n < 0) return std::unexpected(errno);
    return static_cast<std::size_t>(n);
}

bool is_open(int fd) {
    return ::fcntl(fd, F_GETFD) != -1;
}

bool is_nonblocking(int fd) {
    return (::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0;
}

void set_nonblocking(int fd) {
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
}

// A blocking pipe: AsyncFd is the one to make it non-blocking.
struct Pipe {
    int read_end  = -1;
    int write_end = -1;
    Pipe() {
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0) throw std::system_error(errno, std::generic_category(), "pipe2");
        read_end  = fds[0];
        write_end = fds[1];
    }
    ~Pipe() {
        if (read_end != -1)  ::close(read_end);
        if (write_end != -1) ::close(write_end);
    }
    /// Hands the read end to an AsyncFd, which then owns it.
    int give_read_end() { return std::exchange(read_end, -1); }
};

// A connected AF_UNIX stream pair. `a` is handed to an AsyncFd; `b` is the peer.
struct StreamPair {
    int a = -1;
    int b = -1;
    StreamPair() {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
            throw std::system_error(errno, std::generic_category(), "socketpair");
        a = sv[0];
        b = sv[1];
    }
    ~StreamPair() {
        if (a != -1) ::close(a);
        if (b != -1) ::close(b);
    }
    int give_a() { return std::exchange(a, -1); }
    void close_b() { ::close(std::exchange(b, -1)); }
};

// Writes to `fd` (non-blocking) until its send buffer is full.
void fill(int fd) {
    std::array<std::byte, 4096> chunk{};
    while (send_some(fd, chunk.data(), chunk.size())) {}
}

// Reads everything available from the non-blocking `fd`.
void drain(int fd) {
    std::array<std::byte, 4096> chunk{};
    while (true) {
        auto n = read_some(fd, chunk.data(), chunk.size());
        if (!n || *n == 0) return;
    }
}

Coro<std::byte> read_one(AsyncFd& afd) {
    std::byte b{};
    const int fd = afd.fd();
    const std::size_t n = co_await afd.read_io([fd, &b] { return read_some(fd, &b, 1); });
    EXPECT_EQ(n, 1u);
    co_return b;
}

Coro<void> send_one(AsyncFd& afd) {
    const std::byte b{0x5a};
    const int fd = afd.fd();
    (void)co_await afd.write_io([fd, &b] { return send_some(fd, &b, 1); });
}

} // namespace

template<typename Traits>
class AsyncFdTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(AsyncFdTest, AllExecutors);

// ---------------------------------------------------------------------------
// The readiness API
// ---------------------------------------------------------------------------

TYPED_TEST(AsyncFdTest, ReadIoWaitsForReadable) {
    Pipe p;
    const std::byte got = this->traits.rt.block_on([](Pipe& p) -> Coro<std::byte> {
        AsyncFd afd(p.give_read_end(), Interest::read());
        auto reader = spawn(read_one(afd));
        co_await sleep_for(20ms);   // reader waiting
        const std::byte b{0x42};
        EXPECT_EQ(::write(p.write_end, &b, 1), 1);
        co_return co_await reader;
    }(p));
    EXPECT_EQ(got, std::byte{0x42});
}

TYPED_TEST(AsyncFdTest, WriteIoWaitsForWritable) {
    StreamPair s;
    set_nonblocking(s.b);
    this->traits.rt.block_on([](StreamPair& s) -> Coro<void> {
        AsyncFd afd(s.give_a());
        fill(afd.fd());
        // Atomic: on a multi-threaded executor the writer can run on another worker,
        // and the only ordering between its write and the reads here is the socket
        // (drain() -> epoll), which isn't a happens-before edge.
        std::atomic<bool> sent{false};
        auto writer = spawn([](AsyncFd& afd, std::atomic<bool>& sent) -> Coro<void> {
            co_await send_one(afd);
            sent = true;
        }(afd, sent));
        co_await sleep_for(20ms);
        EXPECT_FALSE(sent) << "the send buffer was full";

        drain(s.b);
        auto r = co_await timeout(5s, std::move(writer));
        EXPECT_EQ(r.index(), 0u);
        EXPECT_TRUE(sent);
    }(s));
}

// The operation's errno, if it is not would-block, fails the future.
TYPED_TEST(AsyncFdTest, OtherErrnoFails) {
    Pipe p;
    this->traits.rt.block_on([](Pipe& p) -> Coro<void> {
        AsyncFd afd(p.give_read_end(), Interest::read());
        int calls = 0;
        int code  = 0;
        try {
            (void)co_await afd.read_io([&calls]() -> std::expected<int, int> {
                ++calls;
                return std::unexpected(EIO);
            });
        } catch (const std::system_error& e) {
            code = e.code().value();
        }
        EXPECT_EQ(code, EIO);
        EXPECT_EQ(calls, 1);
    }(p));
}

// EPOLLHUP/EPOLLERR mark both directions ready; each waiter's own syscall then
// reports what happened: end of file for the reader, EPIPE for the writer.
//
// `b` is closed with `fill()`'s data unread, so Linux also gives `a` a one-time
// ECONNRESET, which whichever syscall runs first reports instead of its own result.
TYPED_TEST(AsyncFdTest, HangUpWakesBothDirections) {
    StreamPair s;
    this->traits.rt.block_on([](StreamPair& s) -> Coro<void> {
        AsyncFd afd(s.give_a());
        fill(afd.fd());

        // 0 at end of file, else the errno; -1 if a byte arrived (it can't).
        auto reader = spawn([](AsyncFd& afd) -> Coro<int> {
            std::byte b{};
            const int fd = afd.fd();
            try {
                const std::size_t n =
                    co_await afd.read_io([fd, &b] { return read_some(fd, &b, 1); });
                co_return n == 0 ? 0 : -1;
            } catch (const std::system_error& e) {
                co_return e.code().value();
            }
        }(afd));
        auto writer = spawn([](AsyncFd& afd) -> Coro<int> {
            try {
                co_await send_one(afd);
            } catch (const std::system_error& e) {
                co_return e.code().value();
            }
            co_return 0;
        }(afd));
        co_await sleep_for(20ms);   // both waiting

        s.close_b();
        auto read_result = co_await timeout(5s, std::move(reader));
        auto write_result = co_await timeout(5s, std::move(writer));
        // EXPECT, not ASSERT: ASSERT's plain `return` doesn't compile in a coroutine.
        EXPECT_EQ(read_result.index(), 0u) << "the hang-up didn't wake the reader";
        EXPECT_EQ(write_result.index(), 0u) << "the hang-up didn't wake the writer";
        if (read_result.index() != 0 || write_result.index() != 0) co_return;
        const int read_code  = std::get<0>(read_result).value;
        const int write_code = std::get<0>(write_result).value;
        EXPECT_TRUE(read_code == 0 || read_code == ECONNRESET) << "reader got " << read_code;
        EXPECT_TRUE(write_code == EPIPE || write_code == ECONNRESET) << "writer got " << write_code;
        EXPECT_FALSE(read_code == ECONNRESET && write_code == ECONNRESET)
            << "the reset is reported once";
    }(s));
}

// ---------------------------------------------------------------------------
// Ownership of the fd
// ---------------------------------------------------------------------------

TEST(AsyncFd, ConstructorSetsNonBlocking) {
    SingleThreadRuntime rt;
    Pipe p;
    ASSERT_FALSE(is_nonblocking(p.read_end));
    rt.block_on([](Pipe& p) -> Coro<void> {
        AsyncFd afd(p.give_read_end(), Interest::read());
        EXPECT_TRUE(is_nonblocking(afd.fd()));
        co_return;
    }(p));
}

// Deregistered before closed. A stale epoll registration would outlive the close
// while another fd still refers to the same open file description (here `keep`), and
// registering that file again under the same fd number would then fail with EEXIST.
TEST(AsyncFd, DestructorDeregistersThenCloses) {
    SingleThreadRuntime rt;
    Pipe p;
    rt.block_on([](Pipe& p) -> Coro<void> {
        const int fd   = p.give_read_end();
        const int keep = ::dup(fd);
        { AsyncFd afd(fd, Interest::read()); }
        EXPECT_FALSE(is_open(fd));

        const int again = ::dup(keep);   // the lowest free number: fd's
        ::close(keep);
        EXPECT_EQ(again, fd);
        EXPECT_NO_THROW({ AsyncFd again_afd(again, Interest::read()); });
        co_return;
    }(p));
}

TEST(AsyncFd, ReleaseReturnsOpenFd) {
    SingleThreadRuntime rt;
    Pipe p;
    rt.block_on([](Pipe& p) -> Coro<void> {
        const int fd = p.give_read_end();
        AsyncFd afd(fd, Interest::read());
        const int released = afd.release();
        EXPECT_EQ(released, fd);
        EXPECT_EQ(afd.fd(), -1);
        EXPECT_TRUE(is_open(released));
        EXPECT_TRUE(is_nonblocking(released));
        // Deregistered: registering the same fd again succeeds.
        EXPECT_NO_THROW({ AsyncFd again_afd(released, Interest::read()); });
        co_return;
    }(p));
}

// epoll refuses a regular file with EPERM. The AsyncFd owned the fd from the start,
// so the failed constructor closed it.
TEST(AsyncFd, RegularFileIsRefused) {
    SingleThreadRuntime rt;
    rt.block_on([]() -> Coro<void> {
        char path[] = "/tmp/coro_test_async_fd_XXXXXX";
        const int fd = ::mkstemp(path);
        ::unlink(path);
        EXPECT_NE(fd, -1);
        int code = 0;
        try {
            [[maybe_unused]] AsyncFd afd(fd);
        } catch (const std::system_error& e) {
            code = e.code().value();
        }
        EXPECT_EQ(code, EPERM);
        EXPECT_FALSE(is_open(fd));
        co_return;
    }());
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

TEST(AsyncFd, ShutdownFailsPendingFuture) {
    SingleThreadRuntime rt;
    Pipe p;
    rt.block_on([](Pipe& p) -> Coro<void> {
        AsyncFd afd(p.give_read_end(), Interest::read());
        int error = 0;
        auto waiter = spawn([](AsyncFd& afd, int& error) -> Coro<void> {
            try {
                (void)co_await read_one(afd);
            } catch (const std::system_error& e) {
                error = e.code().value();
            }
        }(afd, error));
        co_await sleep_for(20ms);   // waiting

        current_runtime().io_driver().shutdown();
        co_await waiter;
        EXPECT_EQ(error, ECANCELED);
    }(p));
}
