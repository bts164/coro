#include <gtest/gtest.h>
#include <coro/io/file.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <array>
#include <filesystem>
#include <string>
#include <cstddef>

using namespace coro;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

static_assert(Future<File::OpenFuture>);
static_assert(Future<File::ReadFuture>);
static_assert(Future<File::WriteFuture>);

// ---------------------------------------------------------------------------
// Helper: temporary file RAII wrapper
// ---------------------------------------------------------------------------

class TempFile {
public:
    explicit TempFile(std::string name)
        : m_path(fs::temp_directory_path() / name) {
        // Remove if already exists
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
// Basic open/read/write/close
// ---------------------------------------------------------------------------

TEST(FileTest, BasicOpenWriteReadClose) {
    TempFile temp("test_basic.txt");

    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        // TODO: uncomment when implementation is complete
        // Write data to a new file
        // auto file = co_await File::open(temp.path_string(),
        //     FileMode::Write | FileMode::Create | FileMode::Truncate);
        //
        // std::string data = "Hello, File I/O!";
        // auto data_bytes = std::as_bytes(std::span(data));
        // std::size_t written = co_await file.write(data_bytes);
        // EXPECT_EQ(written, data.size());
        //
        // // Drop file to trigger close

        // Re-open read-only and read back
        // auto file_read = co_await File::open(temp.path_string(), FileMode::Read);
        // std::array<std::byte, 128> buf;
        // std::size_t n = co_await file_read.read(std::span(buf));
        // EXPECT_EQ(n, data.size());
        //
        // std::string read_back(reinterpret_cast<const char*>(buf.data()), n);
        // EXPECT_EQ(read_back, data);

        co_return;
    }());
}

// ---------------------------------------------------------------------------
// Read EOF
// ---------------------------------------------------------------------------

TEST(FileTest, ReadEOF) {
    TempFile temp("test_eof.txt");

    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        // TODO: uncomment when implementation is complete
        // Create an empty file
        // auto file = co_await File::open(temp.path_string(),
        //     FileMode::Write | FileMode::Create);
        // // Drop file to close
        //
        // // Re-open and try to read
        // auto file_read = co_await File::open(temp.path_string(), FileMode::Read);
        // std::array<std::byte, 16> buf;
        // std::size_t n = co_await file_read.read(std::span(buf));
        // EXPECT_EQ(n, 0);  // EOF

        co_return;
    }());
}

// ---------------------------------------------------------------------------
// Write to non-existent file without Create flag
// ---------------------------------------------------------------------------

TEST(FileTest, OpenNonExistentFileForWrite) {
    TempFile temp("test_nonexistent.txt");
    // Ensure it doesn't exist
    fs::remove(temp.path());

    Runtime rt;
    bool threw = false;
    try {
        rt.block_on([]() -> Coro<void> {
            // TODO: uncomment when implementation is complete
            // Should throw because file doesn't exist and Create is not set
            // auto file = co_await File::open(temp.path_string(), FileMode::Write);

            co_return;
        }());
    } catch (const std::system_error&) {
        threw = true;
    }

    // EXPECT_TRUE(threw);  // TODO: uncomment when implementation is complete
    (void)threw;  // suppress unused variable warning
}

// ---------------------------------------------------------------------------
// Read from write-only file
// ---------------------------------------------------------------------------

TEST(FileTest, ReadFromWriteOnlyFile) {
    TempFile temp("test_write_only.txt");

    Runtime rt;
    bool threw = false;
    try {
        rt.block_on([]() -> Coro<void> {
            // TODO: uncomment when implementation is complete
            // auto file = co_await File::open(temp.path_string(),
            //     FileMode::Write | FileMode::Create);
            //
            // std::array<std::byte, 16> buf;
            // std::size_t n = co_await file.read(std::span(buf));  // should fail

            co_return;
        }());
    } catch (const std::system_error&) {
        threw = true;
    }

    // EXPECT_TRUE(threw);  // TODO: uncomment when implementation is complete
    (void)threw;
}

// ---------------------------------------------------------------------------
// Positional I/O (pread/pwrite style)
// ---------------------------------------------------------------------------

TEST(FileTest, PositionalIO) {
    TempFile temp("test_positional.txt");

    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        // TODO: uncomment when implementation is complete
        // Open file for read/write
        // auto file = co_await File::open(temp.path_string(),
        //     FileMode::ReadWrite | FileMode::Create | FileMode::Truncate);
        //
        // // Write "AAAA" at offset 0
        // std::string data_a = "AAAA";
        // auto bytes_a = std::as_bytes(std::span(data_a));
        // co_await file.write_at(bytes_a, 0);
        //
        // // Write "BBBB" at offset 100 (leaves a hole)
        // std::string data_b = "BBBB";
        // auto bytes_b = std::as_bytes(std::span(data_b));
        // co_await file.write_at(bytes_b, 100);
        //
        // // Read at offset 100
        // std::array<std::byte, 4> buf;
        // std::size_t n = co_await file.read_at(std::span(buf), 100);
        // EXPECT_EQ(n, 4);
        //
        // std::string read_back(reinterpret_cast<const char*>(buf.data()), n);
        // EXPECT_EQ(read_back, "BBBB");

        co_return;
    }());
}

// ---------------------------------------------------------------------------
// Large file (stress test)
// ---------------------------------------------------------------------------

TEST(FileTest, LargeFile) {
    TempFile temp("test_large.txt");

    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        // TODO: uncomment when implementation is complete
        // Write 10 MB in 1KB chunks
        // constexpr std::size_t chunk_size = 1024;
        // constexpr std::size_t total_chunks = 10 * 1024;  // 10 MB
        //
        // auto file = co_await File::open(temp.path_string(),
        //     FileMode::Write | FileMode::Create | FileMode::Truncate);
        //
        // std::array<std::byte, chunk_size> chunk;
        // std::fill(chunk.begin(), chunk.end(), std::byte{'X'});
        //
        // for (std::size_t i = 0; i < total_chunks; ++i) {
        //     std::size_t written = co_await file.write(std::span(chunk));
        //     EXPECT_EQ(written, chunk_size);
        // }
        //
        // // Drop file to close
        //
        // // Re-open and verify size
        // auto file_read = co_await File::open(temp.path_string(), FileMode::Read);
        // std::size_t total_read = 0;
        // std::array<std::byte, chunk_size> read_buf;
        //
        // while (true) {
        //     std::size_t n = co_await file_read.read(std::span(read_buf));
        //     if (n == 0) break;
        //     total_read += n;
        // }
        //
        // EXPECT_EQ(total_read, chunk_size * total_chunks);

        co_return;
    }());
}

// ---------------------------------------------------------------------------
// Cancellation (drop future mid-flight)
// ---------------------------------------------------------------------------

TEST(FileTest, Cancellation) {
    TempFile temp("test_cancel.txt");

    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        // TODO: uncomment when implementation is complete
        // Create a large file
        // auto file_write = co_await File::open(temp.path_string(),
        //     FileMode::Write | FileMode::Create);
        // std::array<std::byte, 1024 * 1024> large_buf;  // 1 MB
        // std::fill(large_buf.begin(), large_buf.end(), std::byte{'X'});
        // co_await file_write.write(std::span(large_buf));
        //
        // // Drop file to close
        //
        // // Open for reading
        // auto file_read = co_await File::open(temp.path_string(), FileMode::Read);
        //
        // // Start a read but drop the future before it completes
        // std::atomic<bool> callback_fired{false};
        // {
        //     std::array<std::byte, 1024 * 1024> read_buf;
        //     auto read_future = file_read.read(std::span(read_buf));
        //     // Drop read_future here — should trigger cancellation
        // }
        //
        // // Verify no crash occurred
        // // (callback_fired would be set by a canary in the state if it fired after cancel)

        co_return;
    }());
}

// ---------------------------------------------------------------------------
// Multiple files concurrently
// ---------------------------------------------------------------------------

TEST(FileTest, MultipleConcurrentFiles) {
    Runtime rt;
    rt.block_on([]() -> Coro<void> {
        // TODO: uncomment when implementation is complete
        // Open 10 temp files, write different data to each, read back and verify
        // constexpr int num_files = 10;
        // std::vector<TempFile> temps;
        // for (int i = 0; i < num_files; ++i) {
        //     temps.emplace_back(std::string("test_concurrent_") + std::to_string(i) + ".txt");
        // }
        //
        // // Write phase
        // for (int i = 0; i < num_files; ++i) {
        //     auto file = co_await File::open(temps[i].path_string(),
        //         FileMode::Write | FileMode::Create);
        //     std::string data = "File " + std::to_string(i);
        //     auto bytes = std::as_bytes(std::span(data));
        //     co_await file.write(bytes);
        // }
        //
        // // Read phase
        // for (int i = 0; i < num_files; ++i) {
        //     auto file = co_await File::open(temps[i].path_string(), FileMode::Read);
        //     std::array<std::byte, 128> buf;
        //     std::size_t n = co_await file.read(std::span(buf));
        //
        //     std::string read_back(reinterpret_cast<const char*>(buf.data()), n);
        //     std::string expected = "File " + std::to_string(i);
        //     EXPECT_EQ(read_back, expected);
        // }

        co_return;
    }());
}
