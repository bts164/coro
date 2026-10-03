// Pipe (named FIFO) on the IoDriver (doc/design/pipe_streaming.md).

#include <gtest/gtest.h>
#include <coro/io/pipe.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <coro/coro.h>
#include <coro/sync/sleep.h>
#include <coro/sync/timeout.h>
#include <coro/task/spawn_builder.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace coro;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<Coro<Pipe>>);
static_assert(Future<PipeReadFuture<std::string, false>>);
static_assert(Future<PipeReadFuture<std::string, true>>);
static_assert(Future<PipeWriteFuture<std::string>>);
// Leaf futures: safe to drop mid-wait, so they expose no cancel().
static_assert(!Cancellable<PipeReadFuture<std::string, false>>);
static_assert(!Cancellable<PipeReadFuture<std::string, true>>);
static_assert(!Cancellable<PipeWriteFuture<std::string>>);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// A FIFO path in the temp directory, removed on construction and destruction. The pid
// keeps concurrent test processes apart.
class TempPipe {
public:
    explicit TempPipe(const std::string& name)
        : m_path(fs::temp_directory_path() /
                 ("coro_test_pipe_" + std::to_string(::getpid()) + "_" + name + ".fifo")) {
        fs::remove(m_path);
    }
    ~TempPipe() { fs::remove(m_path); }

    std::string path() const { return m_path.string(); }

private:
    fs::path m_path;
};

// Ignores SIGPIPE for its lifetime, restoring the previous disposition.
class IgnoreSigpipe {
public:
    IgnoreSigpipe() : m_old(std::signal(SIGPIPE, SIG_IGN)) {}
    ~IgnoreSigpipe() { std::signal(SIGPIPE, m_old); }

private:
    void (*m_old)(int);
};

Coro<std::string> open_and_read_exact(std::string path, std::size_t n) {
    Pipe reader = co_await Pipe::open(path, PipeMode::Read);
    auto [got, buf] = co_await reader.read_exact(std::string(n, '\0'));
    buf.resize(got);
    co_return buf;
}

// Reader spawned first: its open() waits until the writer has written.
Coro<std::string> write_and_read(std::string path, std::string msg) {
    co_await Pipe::create(path);
    auto reader = coro::spawn(open_and_read_exact(path, msg.size()));
    Pipe writer = co_await Pipe::open(path, PipeMode::Write);
    co_await writer.write(std::move(msg));
    co_return co_await reader;
}

std::byte pattern_at(std::size_t i) {
    return static_cast<std::byte>((i * 31) % 251);
}

Coro<void> write_pattern(std::string path, std::size_t bytes) {
    Pipe writer = co_await Pipe::open(path, PipeMode::Write);
    std::vector<std::byte> data(bytes);
    for (std::size_t i = 0; i < bytes; ++i) data[i] = pattern_at(i);
    auto back = co_await writer.write(std::move(data));
    EXPECT_EQ(back.size(), bytes);   // write() returns the buffer intact
}

// Returns the number of bytes that matched the pattern.
Coro<std::size_t> large_transfer(std::string path, std::size_t bytes) {
    co_await Pipe::create(path);
    auto writer = coro::spawn(write_pattern(path, bytes));
    Pipe reader = co_await Pipe::open(path, PipeMode::Read);
    auto [n, buf] = co_await reader.read_exact(std::vector<std::byte>(bytes));
    co_await writer;
    std::size_t matching = 0;
    for (std::size_t i = 0; i < n; ++i) matching += buf[i] == pattern_at(i) ? 1 : 0;
    co_return matching;
}

// Several times the 64 KiB Linux pipe buffer.
constexpr std::size_t kLargeTransfer = 1024 * 1024;

// ---------------------------------------------------------------------------
// create
// ---------------------------------------------------------------------------

TEST(PipeTest, CreateMakesFifo) {
    TempPipe temp("create");
    Runtime rt;
    rt.block_on(Pipe::create(temp.path()));
    EXPECT_TRUE(fs::is_fifo(temp.path()));
}

TEST(PipeTest, CreateIdempotent) {
    TempPipe temp("idempotent");
    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        co_await Pipe::create(path);
        co_await Pipe::create(path);   // EEXIST is ignored
    }(temp.path()));
    EXPECT_TRUE(fs::is_fifo(temp.path()));
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

TEST(PipeTest, WriteAndReadWorkStealing) {
    TempPipe temp("rw_ws");
    Runtime rt;
    EXPECT_EQ(rt.block_on(write_and_read(temp.path(), "hello pipe")), "hello pipe");
}

TEST(PipeTest, WriteAndReadCurrentThread) {
    TempPipe temp("rw_ct");
    Runtime rt(1);
    EXPECT_EQ(rt.block_on(write_and_read(temp.path(), "hello pipe")), "hello pipe");
}

TEST(PipeTest, ReaderGetsEofAfterWriterCloses) {
    TempPipe temp("eof");
    Runtime rt;
    const std::size_t n = rt.block_on([](std::string path) -> Coro<std::size_t> {
        co_await Pipe::create(path);
        auto reader = coro::spawn([](std::string p) -> Coro<std::size_t> {
            Pipe r = co_await Pipe::open(p, PipeMode::Read);
            auto [first, buf] = co_await r.read(std::array<std::byte, 32>{});
            EXPECT_EQ(first, 1u);
            auto [second, buf2] = co_await r.read(std::array<std::byte, 32>{});
            co_return second;
        }(path));
        {
            Pipe writer = co_await Pipe::open(path, PipeMode::Write);
            co_await writer.write(std::string("x"));
        }   // the writer closes: the reader's next read sees EOF
        auto got = co_await coro::timeout(5s, std::move(reader));
        co_return got.index() == 0 ? std::get<0>(got).value : 99u;
    }(temp.path()));
    EXPECT_EQ(n, 0u);
}

TEST(PipeTest, MultipleWritesInOrder) {
    TempPipe temp("ordered");
    Runtime rt;
    const std::string got = rt.block_on([](std::string path) -> Coro<std::string> {
        co_await Pipe::create(path);
        auto reader = coro::spawn(open_and_read_exact(path, 9));
        Pipe writer = co_await Pipe::open(path, PipeMode::Write);
        co_await writer.write(std::string("foo"));
        co_await writer.write(std::string("bar"));
        co_await writer.write(std::string("baz"));
        co_return co_await reader;
    }(temp.path()));
    EXPECT_EQ(got, "foobarbaz");
}

// One write() far larger than the pipe buffer: it completes in pieces, each waiting for
// write readiness, and every byte arrives in order.
TEST(PipeTest, LargeWriteCompletesInPiecesWorkStealing) {
    TempPipe temp("large_ws");
    Runtime rt;
    EXPECT_EQ(rt.block_on(large_transfer(temp.path(), kLargeTransfer)), kLargeTransfer);
}

TEST(PipeTest, LargeWriteCompletesInPiecesCurrentThread) {
    TempPipe temp("large_ct");
    Runtime rt(1);
    EXPECT_EQ(rt.block_on(large_transfer(temp.path(), kLargeTransfer)), kLargeTransfer);
}

TEST(PipeTest, ReadWriteModeNeverWaits) {
    TempPipe temp("rdwr");
    Runtime rt;
    const std::string got = rt.block_on([](std::string path) -> Coro<std::string> {
        co_await Pipe::create(path);
        Pipe p = co_await Pipe::open(path, PipeMode::ReadWrite);
        co_await p.write(std::string("loopback"));
        auto [n, buf] = co_await p.read_exact(std::string(8, '\0'));
        EXPECT_EQ(n, 8u);
        co_return buf;
    }(temp.path()));
    EXPECT_EQ(got, "loopback");
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

// The writer's open() starts before any reader exists, so it has to retry on its timer
// until the reader turns up.
TEST(PipeTest, WriterOpenWaitsForReader) {
    TempPipe temp("writer_waits");
    Runtime rt;
    const std::string got = rt.block_on([](std::string path) -> Coro<std::string> {
        co_await Pipe::create(path);
        auto writer = coro::spawn([](std::string p) -> Coro<void> {
            Pipe w = co_await Pipe::open(p, PipeMode::Write);
            co_await w.write(std::string("late reader"));
        }(path));
        co_await coro::sleep_for(30ms);   // the writer is retrying by now
        auto got = co_await coro::timeout(5s, open_and_read_exact(path, 11));
        co_await writer;
        co_return got.index() == 0 ? std::move(std::get<0>(got).value) : std::string("timed out");
    }(temp.path()));
    EXPECT_EQ(got, "late reader");
}

// A writer that opens and closes without writing still ends the reader's open(), and
// the first read is EOF.
TEST(PipeTest, ReaderOpenSeesWriterThatWroteNothing) {
    TempPipe temp("silent_writer");
    Runtime rt;
    const std::size_t n = rt.block_on([](std::string path) -> Coro<std::size_t> {
        co_await Pipe::create(path);
        auto reader = coro::spawn([](std::string p) -> Coro<std::size_t> {
            Pipe r = co_await Pipe::open(p, PipeMode::Read);
            auto [n, buf] = co_await r.read(std::string(16, '\0'));
            co_return n;
        }(path));
        { [[maybe_unused]] Pipe w = co_await Pipe::open(path, PipeMode::Write); }
        auto got = co_await coro::timeout(5s, std::move(reader));
        co_return got.index() == 0 ? std::get<0>(got).value : 99u;
    }(temp.path()));
    EXPECT_EQ(n, 0u);
}

TEST(PipeTest, OpenNonExistentThrows) {
    TempPipe temp("noent");
    Runtime rt;
    EXPECT_THROW((void)rt.block_on(Pipe::open(temp.path(), PipeMode::Read)), std::system_error);
    EXPECT_THROW((void)rt.block_on(Pipe::open(temp.path(), PipeMode::Write)), std::system_error);
    EXPECT_THROW((void)rt.block_on(Pipe::open(temp.path(), PipeMode::ReadWrite)),
                 std::system_error);
}

// A Write-mode open() with no reader, dropped mid-retry by a timeout, leaves nothing
// behind: the FIFO still works for a later reader and writer.
TEST(PipeTest, DroppedOpenLeavesNothingOpen) {
    TempPipe temp("dropped_open");
    Runtime rt;
    const std::string got = rt.block_on([](std::string path) -> Coro<std::string> {
        co_await Pipe::create(path);
        auto timed = co_await coro::timeout(20ms, Pipe::open(path, PipeMode::Write));
        EXPECT_EQ(timed.index(), 1u);
        auto timed_read = co_await coro::timeout(20ms, Pipe::open(path, PipeMode::Read));
        EXPECT_EQ(timed_read.index(), 1u);
        co_return co_await write_and_read(path, "after-drop");
    }(temp.path()));
    EXPECT_EQ(got, "after-drop");
}

// A CurrentThreadExecutor with a caller-supplied parker never turns the IoDriver, so a
// Pipe could never be woken there: open() refuses instead of hanging later. create()
// needs no driver.
TEST(PipeTest, OpenThrowsWithoutDriver) {
    TempPipe temp("no_driver");
    Runtime rt(std::in_place_type<CurrentThreadExecutor>,
               std::make_unique<PollingParker>([] {}));
    rt.block_on(Pipe::create(temp.path()));
    EXPECT_THROW((void)rt.block_on(Pipe::open(temp.path(), PipeMode::ReadWrite)),
                 std::logic_error);
}

// ---------------------------------------------------------------------------
// Errors, dropping
// ---------------------------------------------------------------------------

// A write after the reader closed fails with EPIPE. FIFO writes can't suppress SIGPIPE,
// so the test ignores it (as Pipe's documentation tells applications to).
TEST(PipeTest, WriteAfterReaderClosedThrowsEpipe) {
    TempPipe temp("epipe");
    IgnoreSigpipe ignore;
    Runtime rt;
    const int err = rt.block_on([](std::string path) -> Coro<int> {
        co_await Pipe::create(path);
        auto reader = coro::spawn([](std::string p) -> Coro<void> {
            Pipe r = co_await Pipe::open(p, PipeMode::Read);
            (void)co_await r.read(std::string(16, '\0'));
        }(path));   // the reader closes after its first read
        Pipe writer = co_await Pipe::open(path, PipeMode::Write);
        co_await writer.write(std::string("first"));
        co_await reader;
        try {
            co_await writer.write(std::string("second"));
        } catch (const std::system_error& e) {
            co_return e.code().value();
        }
        co_return 0;
    }(temp.path()));
    EXPECT_EQ(err, EPIPE);
}

// A read() dropped mid-wait consumed nothing; the next read() gets the data.
TEST(PipeTest, DroppedReadLosesNoData) {
    TempPipe temp("dropped_read");
    Runtime rt;
    const std::string got = rt.block_on([](std::string path) -> Coro<std::string> {
        co_await Pipe::create(path);
        Pipe p = co_await Pipe::open(path, PipeMode::ReadWrite);
        auto timed = co_await coro::timeout(20ms, p.read(std::string(64, '\0')));
        EXPECT_EQ(timed.index(), 1u);
        co_await p.write(std::string("after-drop"));
        auto [n, buf] = co_await p.read_exact(std::string(10, '\0'));
        EXPECT_EQ(n, 10u);
        co_return buf;
    }(temp.path()));
    EXPECT_EQ(got, "after-drop");
}
