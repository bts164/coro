// Helpers shared by the Windows sys backend: Winsock startup, error mapping, UTF-8
// conversion, and the ntdll lookups. See src/detail/sys/windows/win.h and nt.h.

#include "nt.h"
#include "win.h"

#include <cerrno>
#include <climits>

namespace coro::detail::sys::win {

void winsock_init() {
    // A function-local static: initialised once, thread-safely, on first use.
    static const int result = [] {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    if (result != 0) throw_errno(wsa_to_errno(result), "WSAStartup");
}

int wsa_to_errno(int wsa_error) noexcept {
    switch (wsa_error) {
    case 0:                     return 0;
    case WSAEWOULDBLOCK:        return EAGAIN;
    case WSAEINPROGRESS:        return EINPROGRESS;
    case WSAEALREADY:           return EALREADY;
    case WSAEINTR:              return EINTR;
    case WSAEBADF:
    case WSAENOTSOCK:           return EBADF;
    case WSAEACCES:             return EACCES;
    case WSAEFAULT:             return EFAULT;
    case WSAEINVAL:             return EINVAL;
    case WSAEMFILE:             return EMFILE;
    case WSAEDESTADDRREQ:       return EDESTADDRREQ;
    case WSAEMSGSIZE:           return EMSGSIZE;
    case WSAEPROTOTYPE:         return EPROTOTYPE;
    case WSAENOPROTOOPT:        return ENOPROTOOPT;
    case WSAEPROTONOSUPPORT:    return EPROTONOSUPPORT;
    case WSAEOPNOTSUPP:         return EOPNOTSUPP;
    case WSAEAFNOSUPPORT:
    case WSAEPFNOSUPPORT:       return EAFNOSUPPORT;
    case WSAEADDRINUSE:         return EADDRINUSE;
    case WSAEADDRNOTAVAIL:      return EADDRNOTAVAIL;
    case WSAENETDOWN:           return ENETDOWN;
    case WSAENETUNREACH:        return ENETUNREACH;
    case WSAENETRESET:          return ENETRESET;
    case WSAECONNABORTED:       return ECONNABORTED;
    case WSAECONNRESET:         return ECONNRESET;
    case WSAENOBUFS:            return ENOBUFS;
    case WSAEISCONN:            return EISCONN;
    case WSAENOTCONN:           return ENOTCONN;
    case WSAESHUTDOWN:          return EPIPE;
    case WSAETIMEDOUT:          return ETIMEDOUT;
    case WSAECONNREFUSED:       return ECONNREFUSED;
    case WSAELOOP:              return ELOOP;
    case WSAENAMETOOLONG:       return ENAMETOOLONG;
    case WSAEHOSTDOWN:
    case WSAEHOSTUNREACH:       return EHOSTUNREACH;
    case WSA_NOT_ENOUGH_MEMORY: return ENOMEM;
    default:                    return EIO;
    }
}

int win32_to_errno(DWORD error) noexcept {
    switch (error) {
    case ERROR_SUCCESS:             return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:        return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:      return EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:      return EEXIST;
    case ERROR_INVALID_HANDLE:      return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:         return ENOMEM;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
    case ERROR_NEGATIVE_SEEK:       return EINVAL;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:    return ENOSPC;
    case ERROR_DIRECTORY:           return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE:return ENAMETOOLONG;
    case ERROR_WRITE_PROTECT:       return EROFS;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:             return EPIPE;
    case ERROR_NOT_SUPPORTED:       return ENOTSUP;
    case ERROR_OPERATION_ABORTED:   return ECANCELED;
    default:                        return EIO;
    }
}

void throw_errno(int err, const char* what) {
    throw std::system_error(err, std::generic_category(), what);
}

std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    if (utf8.size() > static_cast<std::size_t>(INT_MAX)) throw_errno(ENAMETOOLONG, "to_wide");
    const int in_len = static_cast<int>(utf8.size());
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), in_len,
                                        nullptr, 0);
    if (n <= 0) throw_errno(EILSEQ, "to_wide");
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), in_len, out.data(), n);
    return out;
}

std::string to_utf8(const wchar_t* wide) {
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(n - 1));  // drop the terminator
    return out;
}

const NtApi& nt() {
    static const NtApi api = [] {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr) throw_errno(win32_to_errno(::GetLastError()), "ntdll.dll");
        auto get = [ntdll]<typename Fn>(Fn& fn, const char* name) {
            // GetProcAddress returns a generic FARPROC; each type above matches its
            // function's real signature.
            fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(ntdll, name)));
            if (fn == nullptr) throw_errno(ENOSYS, name);
        };
        NtApi a{};
        get(a.NtCreateFile, "NtCreateFile");
        get(a.NtDeviceIoControlFile, "NtDeviceIoControlFile");
        get(a.NtCancelIoFileEx, "NtCancelIoFileEx");
        get(a.RtlNtStatusToDosError, "RtlNtStatusToDosError");
        get(a.NtCreateWaitCompletionPacket, "NtCreateWaitCompletionPacket");
        get(a.NtAssociateWaitCompletionPacket, "NtAssociateWaitCompletionPacket");
        get(a.NtCancelWaitCompletionPacket, "NtCancelWaitCompletionPacket");
        return a;
    }();
    return api;
}

} // namespace coro::detail::sys::win
