// Windows backend for the file seam (include/coro/detail/sys/file.h). See
// doc/design/windows_port.md, "Files".
//
// Windows has no pread(): ReadFile() at an offset also moves a synchronous handle's
// file pointer. So a file's RawFd is the address of a WinFile, which keeps the
// position read() and write() use, and every call passes an explicit offset. Nothing
// but this file looks inside it.

#include <coro/detail/sys/file.h>

#include "win.h"

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <new>

namespace coro::detail::sys {

using namespace win;

namespace {

struct WinFile {
    HANDLE     handle = INVALID_HANDLE_VALUE;
    bool       append = false;
    std::mutex mutex;
    int64_t    position = 0;   // GUARDED BY mutex. Where read()/write() go next
};

WinFile* file_of(RawFd fd) noexcept { return reinterpret_cast<WinFile*>(fd); }

// One call moves at most this much; a short count is allowed.
constexpr std::size_t kMaxChunk = std::size_t{1} << 30;

OVERLAPPED at(int64_t offset) noexcept {
    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>(static_cast<uint64_t>(offset) & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
    return ov;
}

IoResult read_at(HANDLE h, std::byte* data, std::size_t size, int64_t offset) noexcept {
    OVERLAPPED ov = at(offset);
    DWORD n = 0;
    if (!::ReadFile(h, data, static_cast<DWORD>(std::min(size, kMaxChunk)), &n, &ov)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_HANDLE_EOF) return std::size_t{0};
        return std::unexpected(win32_to_errno(err));
    }
    return static_cast<std::size_t>(n);
}

IoResult write_with(HANDLE h, const std::byte* data, std::size_t size, OVERLAPPED ov) noexcept {
    DWORD n = 0;
    if (!::WriteFile(h, data, static_cast<DWORD>(std::min(size, kMaxChunk)), &n, &ov))
        return std::unexpected(win32_to_errno(::GetLastError()));
    return static_cast<std::size_t>(n);
}

} // namespace

std::expected<RawFd, int> file_open(const std::string& path, FileOpenFlags flags) noexcept {
    std::wstring wide;
    try {
        wide = to_wide(path);
    } catch (const std::system_error& e) {
        return std::unexpected(e.code().value());
    } catch (...) {
        return std::unexpected(ENOMEM);
    }

    DWORD access = 0;
    if (flags.read || !flags.write) access |= GENERIC_READ;
    if (flags.write || flags.append) access |= GENERIC_WRITE;

    DWORD disposition;
    if (flags.create && flags.truncate) disposition = CREATE_ALWAYS;
    else if (flags.create)              disposition = OPEN_ALWAYS;
    else if (flags.truncate)            disposition = TRUNCATE_EXISTING;
    else                                disposition = OPEN_EXISTING;

    // Sharing as close to POSIX as Windows allows: others may read, write, rename and
    // delete the file while it is open. Not inheritable (no SECURITY_ATTRIBUTES).
    const HANDLE h = ::CreateFileW(wide.c_str(), access,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::unexpected(win32_to_errno(::GetLastError()));

    auto* file = new (std::nothrow) WinFile;
    if (file == nullptr) {
        ::CloseHandle(h);
        return std::unexpected(ENOMEM);
    }
    file->handle = h;
    file->append = flags.append;
    return reinterpret_cast<RawFd>(file);
}

IoResult file_read(RawFd fd, std::byte* data, std::size_t size, int64_t offset) noexcept {
    WinFile* f = file_of(fd);
    if (offset >= 0) return read_at(f->handle, data, size, offset);
    // The position is held across the read, so concurrent read()s on one File each
    // read their own bytes, as they do on POSIX.
    std::lock_guard lock(f->mutex);
    auto n = read_at(f->handle, data, size, f->position);
    if (n) f->position += static_cast<int64_t>(*n);
    return n;
}

IoResult file_write(RawFd fd, const std::byte* data, std::size_t size,
                    int64_t offset) noexcept {
    WinFile* f = file_of(fd);
    if (offset >= 0) return write_with(f->handle, data, size, at(offset));
    std::lock_guard lock(f->mutex);
    if (f->append) {
        // Offset all ones: write at the end of the file, atomically.
        OVERLAPPED ov{};
        ov.Offset     = 0xFFFFFFFFu;
        ov.OffsetHigh = 0xFFFFFFFFu;
        auto n = write_with(f->handle, data, size, ov);
        if (n) {
            // A synchronous handle's own pointer follows the write: it is now just
            // past the appended bytes, which is where POSIX leaves the position.
            LARGE_INTEGER zero{}, now{};
            if (::SetFilePointerEx(f->handle, zero, &now, FILE_CURRENT))
                f->position = now.QuadPart;
        }
        return n;
    }
    auto n = write_with(f->handle, data, size, at(f->position));
    if (n) f->position += static_cast<int64_t>(*n);
    return n;
}

std::expected<void, int> file_sync(RawFd fd) noexcept {
    if (!::FlushFileBuffers(file_of(fd)->handle))
        return std::unexpected(win32_to_errno(::GetLastError()));
    return {};
}

void file_close(RawFd fd) noexcept {
    WinFile* f = file_of(fd);
    (void)::CloseHandle(f->handle);
    delete f;
}

} // namespace coro::detail::sys
