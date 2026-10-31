#pragma once

// Private to the Windows sys backend: the one place it includes the Windows headers
// (Winsock before <windows.h>, as Winsock requires), and the helpers its seams share.
// See doc/design/windows_port.md.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <coro/detail/sys/poller.h>

#include <cstdint>
#include <string>
#include <system_error>

namespace coro::detail::sys::win {

// INVALID_HANDLE_VALUE is all ones as well, but a pointer, so it can't be checked here.
static_assert(sizeof(RawFd) == sizeof(SOCKET));
static_assert(kInvalidFd == static_cast<RawFd>(INVALID_SOCKET));

inline SOCKET to_socket(RawFd fd) noexcept { return static_cast<SOCKET>(fd); }
inline RawFd  to_fd(SOCKET s) noexcept { return static_cast<RawFd>(s); }

/// Runs WSAStartup() once, before the first socket or name lookup. Never undone:
/// sockets may still be closing during static destruction.
/// @throws std::system_error if Winsock can't be started.
void winsock_init();

/// The errno value for a Winsock error code. A code with no POSIX equivalent maps to
/// EIO.
int wsa_to_errno(int wsa_error) noexcept;

/// The errno value for a Win32 error code (GetLastError()). EIO if there is none.
int win32_to_errno(DWORD error) noexcept;

/// wsa_to_errno(WSAGetLastError()).
inline int last_wsa_errno() noexcept { return wsa_to_errno(::WSAGetLastError()); }

/// Throws std::system_error for errno `err`, in the generic category (see
/// doc/design/windows_port.md, "Error categories").
[[noreturn]] void throw_errno(int err, const char* what);

/// UTF-8 to UTF-16, for the wide Win32 calls. Throws std::system_error (EILSEQ) on
/// malformed input.
std::wstring to_wide(const std::string& utf8);

/// UTF-16 to UTF-8.
std::string to_utf8(const wchar_t* wide);

} // namespace coro::detail::sys::win
