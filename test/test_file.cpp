#include <gtest/gtest.h>
#include <coro/io/file.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstddef>

using namespace coro;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<JoinHandle<File>>);
static_assert(Future<File::ReadFuture>);
static_assert(Future<File::WriteFuture>);

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
        std::string data = "Hello, File I/O!";
        std::size_t written = co_await file.write(std::as_bytes(std::span(data)));
        EXPECT_EQ(written, data.size());
        // Drop file to close, then re-open read-only
        file = co_await File::open(path, FileMode::Read);  // assignment closes previous fd
        std::array<std::byte, 128> buf;
        std::size_t n = co_await file.read(std::span(buf));
        EXPECT_EQ(n, data.size());
        std::string read_back(reinterpret_cast<const char*>(buf.data()), n);
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
        std::array<std::byte, 16> buf;
        std::size_t n = co_await file.read(std::span(buf));
        EXPECT_EQ(n, 0);  // EOF
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

        std::string data_a = "AAAA";
        co_await file.write_at(std::as_bytes(std::span(data_a)), 0);

        std::string data_b = "BBBB";
        co_await file.write_at(std::as_bytes(std::span(data_b)), 100);

        std::array<std::byte, 4> buf;
        std::size_t n = co_await file.read_at(std::span(buf), 100);
        EXPECT_EQ(n, 4);
        std::string read_back(reinterpret_cast<const char*>(buf.data()), n);
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
        std::string chunk(64, 'X');
        co_await file.write(std::as_bytes(std::span(chunk)));
        co_await file.write(std::as_bytes(std::span(chunk)));
        // Rewind and read back.
        file = co_await File::open(path, FileMode::Read);  // closes write fd, opens read
        std::array<std::byte, 64> buf;
        std::size_t total = 0;
        while (true) {
            std::size_t n = co_await file.read(std::span(buf));
            if (n == 0) break;
            total += n;
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
            std::string data = "File" + std::to_string(i);
            co_await file.write(std::as_bytes(std::span(data)));
        }
        // Read phase
        for (int i = 0; i < n; ++i) {
            auto file = co_await File::open(paths[i], FileMode::Read);
            std::array<std::byte, 32> buf;
            std::size_t bytes = co_await file.read(std::span(buf));
            std::string got(reinterpret_cast<const char*>(buf.data()), bytes);
            EXPECT_EQ(got, "File" + std::to_string(i));
        }
    }(N, [&] {
        std::vector<std::string> p;
        for (auto& t : temps) p.push_back(t.path_string());
        return p;
    }()));
}
