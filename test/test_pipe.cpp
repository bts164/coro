#include <gtest/gtest.h>
#include <coro/io/pipe.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/task/spawn_builder.h>
#include <coro/task/spawn_on.h>
#include <array>
#include <filesystem>
#include <string>

using namespace coro;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: temporary FIFO RAII wrapper
// ---------------------------------------------------------------------------

class TempPipe {
public:
    explicit TempPipe(std::string name)
        : m_path(fs::temp_directory_path() / name) {
        fs::remove(m_path);
    }
    ~TempPipe() { fs::remove(m_path); }

    const fs::path& path() const { return m_path; }
    std::string path_string() const { return m_path.string(); }

private:
    fs::path m_path;
};

// ---------------------------------------------------------------------------
// create() makes a FIFO on disk
// ---------------------------------------------------------------------------

TEST(PipeTest, CreateMakesFifo) {
    TempPipe temp("coro_test_pipe_create.fifo");

    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        co_await Pipe::create(path);
        EXPECT_TRUE(fs::exists(path));
    }(temp.path_string()));
}

// ---------------------------------------------------------------------------
// create() is idempotent — calling it twice doesn't throw
// ---------------------------------------------------------------------------

TEST(PipeTest, CreateIdempotent) {
    TempPipe temp("coro_test_pipe_idempotent.fifo");

    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        co_await Pipe::create(path);
        co_await Pipe::create(path);  // EEXIST is silently ignored
    }(temp.path_string()));
}

// ---------------------------------------------------------------------------
// Write then read — spawn reader and writer concurrently so both ends rendezvous
// ---------------------------------------------------------------------------

TEST(PipeTest, WriteAndRead) {
    TempPipe temp("coro_test_pipe_rw.fifo");

    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        co_await Pipe::create(path);

        // Spawn the reader first — it will suspend in Pipe::open until a writer connects.
        auto reader_task = coro::spawn(
            [](std::string p) -> Coro<std::pair<std::size_t, std::array<std::byte, 32>>> {
                Pipe reader = co_await Pipe::open(p, PipeMode::Read);
                co_return co_await reader.read(std::array<std::byte, 32>{});
            }(path)
        );

        // Open the writer — this completes the FIFO rendezvous.
        Pipe writer = co_await Pipe::open(path, PipeMode::Write);
        co_await writer.write(std::string("hello pipe"));

        auto [n, buf] = co_await reader_task;
        EXPECT_EQ(n, 10u);
        std::string got(reinterpret_cast<const char*>(buf.data()), n);
        EXPECT_EQ(got, "hello pipe");
    }(temp.path_string()));
}

// ---------------------------------------------------------------------------
// EOF — reader gets 0 bytes after writer closes
// ---------------------------------------------------------------------------

TEST(PipeTest, ReaderGetsEofAfterWriterCloses) {
    TempPipe temp("coro_test_pipe_eof.fifo");

    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        co_await Pipe::create(path);

        auto reader_task = coro::spawn(
            [](std::string p) -> Coro<std::size_t> {
                Pipe reader = co_await Pipe::open(p, PipeMode::Read);
                // First read gets the data.
                co_await reader.read(std::array<std::byte, 32>{});
                // Second read gets EOF (0 bytes).
                auto [n, buf] = co_await reader.read(std::array<std::byte, 32>{});
                co_return n;
            }(path)
        );

        {
            Pipe writer = co_await Pipe::open(path, PipeMode::Write);
            co_await writer.write(std::string("x"));
            // writer closes here — reader sees EOF on its next read
        }

        std::size_t n = co_await reader_task;
        EXPECT_EQ(n, 0u);
    }(temp.path_string()));
}

// ---------------------------------------------------------------------------
// open() on non-existent path throws
// ---------------------------------------------------------------------------

TEST(PipeTest, OpenNonExistentThrows) {
    TempPipe temp("coro_test_pipe_noent.fifo");
    fs::remove(temp.path());

    Runtime rt;
    bool threw = false;
    try {
        rt.block_on([](std::string path) -> Coro<void> {
            [[maybe_unused]] Pipe p = co_await Pipe::open(path, PipeMode::Read);
        }(temp.path_string()));
    } catch (const std::system_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

// ---------------------------------------------------------------------------
// Pipelined writes — queue multiple writes before awaiting any
// ---------------------------------------------------------------------------

TEST(PipeTest, PipelinedWrites) {
    TempPipe temp("coro_test_pipe_pipeline.fifo");

    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        co_await Pipe::create(path);

        auto reader_task = coro::spawn(
            [](std::string p) -> Coro<std::string> {
                Pipe reader = co_await Pipe::open(p, PipeMode::Read);
                std::string result;
                for (int i = 0; i < 3; ++i) {
                    auto [n, buf] = co_await reader.read(std::array<std::byte, 16>{});
                    result.append(reinterpret_cast<const char*>(buf.data()), n);
                }
                co_return result;
            }(path)
        );

        Pipe writer = co_await Pipe::open(path, PipeMode::Write);

        // Queue three writes before awaiting any — the driver batches them.
        auto h1 = writer.write(std::string("foo"));
        auto h2 = writer.write(std::string("bar"));
        auto h3 = writer.write(std::string("baz"));
        co_await h1;
        co_await h2;
        co_await h3;

        std::string got = co_await reader_task;
        EXPECT_EQ(got, "foobarbaz");
    }(temp.path_string()));
}
