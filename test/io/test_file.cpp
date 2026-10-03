#include <gtest/gtest.h>
#include <coro/io/file.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/parker.h>
#include <coro/coro.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

using namespace coro;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<File::OpenFuture>);
static_assert(Future<File::IoFuture<std::string>>);
static_assert(Future<BlockingHandle<void>>);

// ---------------------------------------------------------------------------
// Helper: temporary file RAII wrapper
// ---------------------------------------------------------------------------

class TempFile {
public:
    explicit TempFile(std::string name)
        : m_path(fs::temp_directory_path() / name) {
        fs::remove(m_path);
    }

    ~TempFile() {
        fs::remove(m_path);
    }

    const fs::path& path() const { return m_path; }
    std::string path_string() const { return m_path.string(); }

private:
    fs::path m_path;
};

// ---------------------------------------------------------------------------
// Basic open / write / read / close
// ---------------------------------------------------------------------------

TEST(FileTest, BasicOpenWriteReadClose) {
    TempFile temp("coro_test_basic.txt");
    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        // Write
        auto file = co_await File::open(path, FileMode::Write | FileMode::Create | FileMode::Truncate);
        auto [written, data] = co_await file.write(std::string("Hello, File I/O!"));
        EXPECT_EQ(written, data.size());
        // Drop file to close, then re-open read-only
        file = co_await File::open(path, FileMode::Read);  // assignment closes previous fd
        auto rr = co_await file.read(std::array<std::byte, 128>{});
        EXPECT_EQ(rr.first, data.size());
        std::string read_back(reinterpret_cast<const char*>(rr.second.data()), rr.first);
        EXPECT_EQ(read_back, data);
    }(temp.path_string()));
}

// ---------------------------------------------------------------------------
// Read EOF
// ---------------------------------------------------------------------------

TEST(FileTest, ReadEOF) {
    TempFile temp("coro_test_eof.txt");
    // Create an empty file synchronously so there is no async-close race.
    { std::ofstream ofs(temp.path_string()); }

    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        auto file = co_await File::open(path, FileMode::Read);
        auto rr = co_await file.read(std::array<std::byte, 16>{});
        EXPECT_EQ(rr.first, 0);  // EOF
    }(temp.path_string()));
}

// ---------------------------------------------------------------------------
// Open non-existent file without Create flag throws
// ---------------------------------------------------------------------------

TEST(FileTest, OpenNonExistentFileForWrite) {
    TempFile temp("coro_test_nonexistent.txt");
    fs::remove(temp.path());

    Runtime rt;
    bool threw = false;
    try {
        rt.block_on([](std::string path) -> Coro<void> {
            auto file = co_await File::open(path, FileMode::Write);  // no Create
        }(temp.path_string()));
    } catch (const std::system_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

// ---------------------------------------------------------------------------
// Positional I/O (pread / pwrite style)
// ---------------------------------------------------------------------------

TEST(FileTest, PositionalIO) {
    TempFile temp("coro_test_positional.txt");
    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        auto file = co_await File::open(
            path, FileMode::ReadWrite | FileMode::Create | FileMode::Truncate);

        co_await file.write_at(std::string("AAAA"), 0);
        co_await file.write_at(std::string("BBBB"), 100);

        auto rr = co_await file.read_at(std::array<std::byte, 4>{}, 100);
        EXPECT_EQ(rr.first, 4);
        std::string read_back(reinterpret_cast<const char*>(rr.second.data()), rr.first);
        EXPECT_EQ(read_back, "BBBB");
    }(temp.path_string()));
}

// ---------------------------------------------------------------------------
// Multiple sequential reads
// ---------------------------------------------------------------------------

TEST(FileTest, MultipleSequentialReads) {
    TempFile temp("coro_test_sequential.txt");
    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        // Write phase: keep file open for reads (no async-close race).
        auto file = co_await File::open(
            path, FileMode::ReadWrite | FileMode::Create | FileMode::Truncate);
        co_await file.write(std::string(64, 'X'));
        co_await file.write(std::string(64, 'X'));
        // Rewind and read back.
        file = co_await File::open(path, FileMode::Read);  // closes write fd, opens read
        std::size_t total = 0;
        while (true) {
            auto rr = co_await file.read(std::array<std::byte, 64>{});
            if (rr.first == 0) break;
            total += rr.first;
        }
        EXPECT_EQ(total, 128u);
    }(temp.path_string()));
}

// ---------------------------------------------------------------------------
// Multiple concurrent files
// ---------------------------------------------------------------------------

TEST(FileTest, MultipleConcurrentFiles) {
    constexpr int N = 5;
    std::vector<TempFile> temps;
    for (int i = 0; i < N; ++i)
        temps.emplace_back("coro_test_concurrent_" + std::to_string(i) + ".txt");

    Runtime rt;
    rt.block_on([](int n, std::vector<std::string> paths) -> Coro<void> {
        // Write phase
        for (int i = 0; i < n; ++i) {
            auto file = co_await File::open(
                paths[i], FileMode::Write | FileMode::Create | FileMode::Truncate);
            co_await file.write("File" + std::to_string(i));
        }
        // Read phase
        for (int i = 0; i < n; ++i) {
            auto file = co_await File::open(paths[i], FileMode::Read);
            auto rr = co_await file.read(std::array<std::byte, 32>{});
            std::string got(reinterpret_cast<const char*>(rr.second.data()), rr.first);
            EXPECT_EQ(got, "File" + std::to_string(i));
        }
    }(N, [&] {
        std::vector<std::string> p;
        for (auto& t : temps) p.push_back(t.path_string());
        return p;
    }()));
}

// ---------------------------------------------------------------------------
// Blocking-pool behaviour
// ---------------------------------------------------------------------------

namespace {

std::string read_whole(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

// The _exact variants loop inside one job; 4 MiB is many syscalls' worth on some
// filesystems and must come back whole, then short at EOF.
TEST(FileTest, ExactVariantsRoundTripLargeBuffer) {
    TempFile temp("coro_test_exact_large.bin");
    Runtime rt;
    auto ok = rt.block_on([](std::string path) -> Coro<bool> {
        constexpr std::size_t kSize = 4u << 20;
        std::vector<std::byte> out(kSize);
        for (std::size_t i = 0; i < kSize; ++i) out[i] = static_cast<std::byte>(i * 31);
        auto file = co_await File::open(path, FileMode::Write | FileMode::Create | FileMode::Truncate);
        auto [written, sent] = co_await file.write_exact(std::move(out));
        EXPECT_EQ(written, kSize);

        auto reader = co_await File::open(path, FileMode::Read);
        auto [got, back] = co_await reader.read_exact(std::vector<std::byte>(kSize + 100));
        EXPECT_EQ(got, kSize);   // stopped short at EOF
        back.resize(got);
        co_return back == sent;
    }(temp.path_string()));
    EXPECT_TRUE(ok);
}

TEST(FileTest, ReadAtExactAndWriteAtExact) {
    TempFile temp("coro_test_at_exact.bin");
    Runtime rt;
    auto got = rt.block_on([](std::string path) -> Coro<std::string> {
        auto file = co_await File::open(path, FileMode::ReadWrite | FileMode::Create | FileMode::Truncate);
        co_await file.write_at_exact(std::string("0123456789"), 0);
        co_await file.write_at_exact(std::string("abc"), 4);
        auto [n, buf] = co_await file.read_at_exact(std::string(6, '\0'), 2);
        buf.resize(n);
        co_return buf;
    }(temp.path_string()));
    EXPECT_EQ(got, "23abc7");
}

TEST(FileTest, AppendWritesAtEnd) {
    TempFile temp("coro_test_append.txt");
    { std::ofstream(temp.path_string()) << "start-"; }
    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        auto file = co_await File::open(path, FileMode::Write | FileMode::Append);
        co_await file.write_exact(std::string("end"));
        co_await file.sync_all();
    }(temp.path_string()));
    EXPECT_EQ(read_whole(temp.path_string()), "start-end");
}

TEST(FileTest, OpenMissingFileThrowsEnoent) {
    TempFile temp("coro_test_missing.txt");
    Runtime rt;
    std::error_code code;
    try {
        rt.block_on([](std::string path) -> Coro<void> {
            auto file = co_await File::open(path, FileMode::Read);
        }(temp.path_string()));
    } catch (const std::system_error& e) {
        code = e.code();
    }
    EXPECT_EQ(code, std::errc::no_such_file_or_directory);
}

// A write started on a File keeps the fd open after the File itself is dropped, and
// still lands.
TEST(FileTest, PendingWriteOutlivesFile) {
    TempFile temp("coro_test_outlive.bin");
    Runtime rt;
    auto written = rt.block_on([](std::string path) -> Coro<std::size_t> {
        auto file = co_await File::open(path, FileMode::Write | FileMode::Create | FileMode::Truncate);
        auto pending = file.write_exact(std::string(1u << 20, 'Z'));
        { File dropped = std::move(file); }
        auto [n, buf] = co_await pending;
        co_return n;
    }(temp.path_string()));
    EXPECT_EQ(written, 1u << 20);
    EXPECT_EQ(read_whole(temp.path_string()), std::string(1u << 20, 'Z'));
}

// Closing is synchronous: reopening right after the drop sees everything written.
TEST(FileTest, ReopenAfterDropSeesWrites) {
    TempFile temp("coro_test_reopen.txt");
    Runtime rt;
    auto got = rt.block_on([](std::string path) -> Coro<std::string> {
        {
            auto file = co_await File::open(path, FileMode::Write | FileMode::Create | FileMode::Truncate);
            co_await file.write_exact(std::string("closed"));
        }
        auto file = co_await File::open(path, FileMode::Read);
        auto [n, buf] = co_await file.read(std::string(32, '\0'));
        buf.resize(n);
        co_return buf;
    }(temp.path_string()));
    EXPECT_EQ(got, "closed");
}

TEST(FileTest, MovedFromFileThrowsLogicError) {
    TempFile temp("coro_test_moved.txt");
    Runtime rt;
    rt.block_on([](std::string path) -> Coro<void> {
        auto file = co_await File::open(path, FileMode::Write | FileMode::Create);
        File other = std::move(file);
        EXPECT_THROW((void)file.write(std::string("x")), std::logic_error);
        EXPECT_THROW((void)file.sync_all(), std::logic_error);
    }(temp.path_string()));
}

// Files need the blocking pool, not the IoDriver: they work on a CurrentThreadExecutor
// with its own (busy-polling) parker, which never turns the driver.
TEST(FileTest, WorksWithoutDriver) {
    TempFile temp("coro_test_no_driver.txt");
    Runtime rt(std::in_place_type<CurrentThreadExecutor>,
               std::make_unique<PollingParker>([] {}));
    auto got = rt.block_on([](std::string path) -> Coro<std::string> {
        auto file = co_await File::open(path, FileMode::ReadWrite | FileMode::Create | FileMode::Truncate);
        co_await file.write_at_exact(std::string("no driver"), 0);
        auto [n, buf] = co_await file.read_at(std::string(32, '\0'), 0);
        buf.resize(n);
        co_return buf;
    }(temp.path_string()));
    EXPECT_EQ(got, "no driver");
}
