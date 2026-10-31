#pragma once

// Private to the Windows sys backend: the undocumented NT calls and AFD structures the
// IOCP/AFD Poller is built on. See doc/design/windows_port.md, "The IOCP/AFD Poller".
//
// The functions are looked up in ntdll.dll at first use rather than linked through
// ntdll.lib, whose contents vary between SDKs. Every one of them exists on Windows 10
// and later; nt() throws if one is missing.

#include "win.h"

#include <winternl.h>

#include <cstdint>

namespace coro::detail::sys::win {

// NTSTATUS values, under names of our own: <ntstatus.h> clashes with <windows.h>.
inline constexpr NTSTATUS kStatusSuccess   = 0;
inline constexpr NTSTATUS kStatusPending   = 0x00000103;
inline constexpr NTSTATUS kStatusCancelled = static_cast<NTSTATUS>(0xC0000120);
inline constexpr NTSTATUS kStatusNotFound  = static_cast<NTSTATUS>(0xC0000225);

/// NtCreateFile's FILE_OPEN disposition: open an existing object.
inline constexpr ULONG kFileOpen = 0x00000001;

// --- AFD (the Winsock kernel driver) ---

/// IOCTL_AFD_POLL: the request select() itself sends to AFD.
inline constexpr ULONG kIoctlAfdPoll = 0x00012024;

// AFD_POLL_* event bits.
inline constexpr ULONG kAfdPollReceive     = 0x0001;
inline constexpr ULONG kAfdPollSend        = 0x0004;
inline constexpr ULONG kAfdPollDisconnect  = 0x0008;  // the peer's FIN
inline constexpr ULONG kAfdPollAbort       = 0x0010;  // a reset
inline constexpr ULONG kAfdPollLocalClose  = 0x0020;  // closed before it was deregistered
inline constexpr ULONG kAfdPollAccept      = 0x0080;
inline constexpr ULONG kAfdPollConnectFail = 0x0100;

struct AfdPollHandleInfo {
    HANDLE   handle;
    ULONG    events;
    NTSTATUS status;
};

struct AfdPollInfo {
    LARGE_INTEGER     timeout;
    ULONG             number_of_handles;  // always 1 here
    ULONG             exclusive;
    AfdPollHandleInfo handles[1];
};

// --- ntdll entry points ---

using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE file, ACCESS_MASK access,
                                        POBJECT_ATTRIBUTES attributes,
                                        PIO_STATUS_BLOCK iosb, PLARGE_INTEGER allocation,
                                        ULONG file_attributes, ULONG share,
                                        ULONG disposition, ULONG options, PVOID ea,
                                        ULONG ea_length);
using NtDeviceIoControlFileFn = NTSTATUS(NTAPI*)(HANDLE file, HANDLE event,
                                                 PIO_APC_ROUTINE apc, PVOID apc_context,
                                                 PIO_STATUS_BLOCK iosb, ULONG code,
                                                 PVOID in, ULONG in_length, PVOID out,
                                                 ULONG out_length);
using NtCancelIoFileExFn = NTSTATUS(NTAPI*)(HANDLE file, PIO_STATUS_BLOCK request,
                                            PIO_STATUS_BLOCK iosb);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(NTSTATUS status);
using NtCreateWaitCompletionPacketFn = NTSTATUS(NTAPI*)(PHANDLE packet, ACCESS_MASK access,
                                                        POBJECT_ATTRIBUTES attributes);
using NtAssociateWaitCompletionPacketFn = NTSTATUS(NTAPI*)(HANDLE packet, HANDLE port,
                                                           HANDLE target, PVOID key,
                                                           PVOID apc_context,
                                                           NTSTATUS status,
                                                           ULONG_PTR information,
                                                           PBOOLEAN already_signaled);
using NtCancelWaitCompletionPacketFn = NTSTATUS(NTAPI*)(HANDLE packet,
                                                        BOOLEAN remove_signaled_packet);

struct NtApi {
    NtCreateFileFn                    NtCreateFile;
    NtDeviceIoControlFileFn           NtDeviceIoControlFile;
    NtCancelIoFileExFn                NtCancelIoFileEx;
    RtlNtStatusToDosErrorFn           RtlNtStatusToDosError;
    NtCreateWaitCompletionPacketFn    NtCreateWaitCompletionPacket;
    NtAssociateWaitCompletionPacketFn NtAssociateWaitCompletionPacket;
    NtCancelWaitCompletionPacketFn    NtCancelWaitCompletionPacket;
};

/// The ntdll entry points, looked up once (thread-safe static initialisation).
/// @throws std::system_error if one is missing.
const NtApi& nt();

} // namespace coro::detail::sys::win
