// IOCP/AFD backend for coro::detail::sys::Poller / PollWaker.
// See include/coro/detail/sys/poller.h for the backend contract, and
// doc/design/windows_port.md, "The IOCP/AFD Poller", for the design.
//
// Each registered socket has a SockState holding at most one IOCTL_AFD_POLL request.
// The request completes on the Poller's completion port once the socket is ready for
// something it asked about. Edges are emulated: a direction that has been reported is
// left out of later requests until rearm() puts it back (IoRegistration::clear_ready()
// calls it on EAGAIN).

#include "nt.h"
#include "win.h"

#include <coro/detail/sys/poller.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef SIO_BSP_HANDLE
#define SIO_BSP_HANDLE _WSAIOR(IOC_WS2, 27)
#endif
#ifndef SIO_BSP_HANDLE_SELECT
#define SIO_BSP_HANDLE_SELECT _WSAIOR(IOC_WS2, 28)
#endif
#ifndef SIO_BSP_HANDLE_POLL
#define SIO_BSP_HANDLE_POLL _WSAIOR(IOC_WS2, 29)
#endif
#ifndef SIO_BASE_HANDLE
#define SIO_BASE_HANDLE _WSAIOR(IOC_WS2, 34)
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace coro::detail::sys {

using namespace win;

namespace {

// Completion keys: what kind of packet the port returned.
constexpr ULONG_PTR kAfdKey   = 1;  // an AFD poll completed; overlapped = its SockState
constexpr ULONG_PTR kWakeKey  = 2;  // PollWaker::wake(); overlapped = the waker's key
constexpr ULONG_PTR kTimerKey = 3;  // the timeout timer expired
constexpr ULONG_PTR kErrorKey = 4;  // a poll couldn't be submitted; overlapped = its SockState

// Entries fetched per GetQueuedCompletionStatusEx(). More are returned by the next call.
constexpr ULONG kMaxEntriesPerWait = 256;
// Sockets per \Device\Afd handle, as in mio and wepoll.
constexpr std::size_t kSocketsPerAfdHandle = 32;
// Extra zero-timeout collections per poll() for polls resubmitted in it: a resubmitted
// poll for a socket that is already ready completes at once, and is reported by this
// poll() rather than the next. Bounded, so a socket that keeps completing can't hold
// the polling thread.
constexpr int kMaxCollectPasses = 4;

constexpr ULONG kReadBits   = kAfdPollReceive | kAfdPollAccept | kAfdPollDisconnect;
constexpr ULONG kWriteBits  = kAfdPollSend;
// Reported whatever the interest, as epoll reports EPOLLERR.
constexpr ULONG kAlwaysBits = kAfdPollAbort | kAfdPollConnectFail;

ULONG afd_bits(Interest interest) noexcept {
    ULONG bits = kAlwaysBits;
    if (interest.readable) bits |= kReadBits;
    if (interest.writable) bits |= kWriteBits;
    return bits;
}

int ntstatus_to_errno(NTSTATUS status) noexcept {
    return win32_to_errno(nt().RtlNtStatusToDosError(status));
}

// A layered service provider may wrap a socket; AFD must be sent the base provider's
// handle. Falls back as mio does, for providers known to break SIO_BASE_HANDLE.
SOCKET base_socket(SOCKET s) {
    SOCKET base  = INVALID_SOCKET;
    DWORD  bytes = 0;
    if (::WSAIoctl(s, SIO_BASE_HANDLE, nullptr, 0, &base, sizeof(base), &bytes, nullptr,
                   nullptr) != SOCKET_ERROR)
        return base;
    const int err = ::WSAGetLastError();
    for (const DWORD ioctl : {DWORD(SIO_BSP_HANDLE_SELECT), DWORD(SIO_BSP_HANDLE_POLL),
                              DWORD(SIO_BSP_HANDLE)}) {
        if (::WSAIoctl(s, ioctl, nullptr, 0, &base, sizeof(base), &bytes, nullptr,
                       nullptr) != SOCKET_ERROR &&
            base != s)
            return base;
    }
    throw_errno(wsa_to_errno(err), "Poller::register_fd (SIO_BASE_HANDLE)");
}

// Milliseconds for a Win32 wait, rounded up so the wait is never shorter than asked.
DWORD ceil_ms(std::chrono::nanoseconds ns) noexcept {
    const auto ms = (ns.count() + 999'999) / 1'000'000;
    return static_cast<DWORD>(std::clamp<long long>(ms, 0, INFINITE - 1));
}

} // namespace

namespace iocp {

// One \Device\Afd handle, shared by up to kSocketsPerAfdHandle sockets.
struct AfdHandle {
    HANDLE      handle = nullptr;
    std::size_t users  = 0;   // GUARDED BY IocpPoller::afd_mutex
};

enum class PollStatus { Idle, Pending, Cancelled };

// One registered socket. Shared by the fd map, the kernel while a poll is in flight
// (`in_flight`, a reference to itself), and the polling thread while it handles one.
struct SockState : std::enable_shared_from_this<SockState> {
    explicit SockState(IocpPoller& owner_) : owner(owner_) {}
    ~SockState();

    IocpPoller& owner;
    RawFd         fd   = kInvalidFd;     // set before the state is shared
    SOCKET        base = INVALID_SOCKET; //   ditto
    AfdHandle*    afd  = nullptr;        //   ditto

    std::mutex      mutex;
    // Written by the kernel while a poll is in flight (status Pending or Cancelled);
    // read by the polling thread once the poll's completion has been dequeued.
    IO_STATUS_BLOCK iosb{};
    AfdPollInfo     poll_info{};
    void*      key       = nullptr;      // GUARDED BY mutex
    ULONG      interest  = 0;            // GUARDED BY mutex. AFD bits of the Interest
    ULONG      armed     = 0;            // GUARDED BY mutex. interest minus directions
                                         //   reported since they were last re-armed
    ULONG      submitted = 0;            // GUARDED BY mutex. Bits of the poll in flight
    PollStatus status    = PollStatus::Idle;  // GUARDED BY mutex
    bool       level     = false;        // GUARDED BY mutex
    bool       deleted   = false;        // GUARDED BY mutex. Deregistered: report nothing
    std::shared_ptr<SockState> in_flight;   // GUARDED BY mutex. Set while the kernel
                                            //   holds this state's address
    std::shared_ptr<SockState> error_ref;   // GUARDED BY mutex. Set while a kErrorKey
                                            //   packet for this state is in the port

    // Touched only by the thread in poll().
    std::uint64_t event_gen    = 0;  // poll() that built an Event for this state
    std::size_t   event_index  = 0;  //   and where in its output
    std::uint64_t resubmit_gen = 0;  // poll() that last resubmitted a level poll
    bool          deferred     = false;  // on IocpPoller::deferred
};

} // namespace iocp

using iocp::AfdHandle;
using iocp::PollStatus;
using iocp::SockState;

struct IocpPoller {
    HANDLE port        = nullptr;
    HANDLE timer       = nullptr;  // high-resolution waitable timer; null if unavailable
    HANDLE wait_packet = nullptr;  // posts kTimerKey when `timer` expires; null if unavailable

    std::mutex                              afd_mutex;
    std::vector<std::unique_ptr<AfdHandle>> afds;      // GUARDED BY afd_mutex

    std::mutex                                             map_mutex;
    std::unordered_map<RawFd, std::shared_ptr<SockState>> sockets;   // GUARDED BY map_mutex

    std::mutex  inflight_mutex;
    std::size_t inflight = 0;   // GUARDED BY inflight_mutex. Packets owed to the port

    // Touched only by the thread in poll().
    std::vector<std::shared_ptr<SockState>> deferred;
    std::uint64_t                           generation = 0;
    OVERLAPPED_ENTRY                        entries[kMaxEntriesPerWait]{};

    // Lock order: a SockState's mutex may be held while taking map_mutex,
    // inflight_mutex or afd_mutex, never the reverse.

    AfdHandle* acquire_afd();
    void       release_afd(AfdHandle* afd) noexcept;
    void       add_inflight() noexcept;
    void       sub_inflight() noexcept;
    std::size_t inflight_count() noexcept;

    int  submit(SockState& s) noexcept;
    void cancel(SockState& s) noexcept;
    void update(SockState& s) noexcept;
    void post_error(SockState& s) noexcept;

    std::shared_ptr<SockState> find(RawFd fd);
};

iocp::SockState::~SockState() {
    if (afd != nullptr) owner.release_afd(afd);
}

// ---------------------------------------------------------------------------
// AFD handles and in-flight accounting
// ---------------------------------------------------------------------------

AfdHandle* IocpPoller::acquire_afd() {
    std::lock_guard lock(afd_mutex);
    for (auto& afd : afds) {
        if (afd->users < kSocketsPerAfdHandle) {
            ++afd->users;
            return afd.get();
        }
    }

    static wchar_t kName[] = L"\\Device\\Afd\\Coro";  // the text after "Afd\" is ignored
    UNICODE_STRING name{};
    name.Buffer        = kName;
    name.Length        = static_cast<USHORT>((sizeof(kName) / sizeof(wchar_t) - 1) * sizeof(wchar_t));
    name.MaximumLength = static_cast<USHORT>(sizeof(kName));
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length     = sizeof(attributes);
    attributes.ObjectName = &name;

    HANDLE          handle = nullptr;
    IO_STATUS_BLOCK iosb{};
    const NTSTATUS status = nt().NtCreateFile(&handle, SYNCHRONIZE, &attributes, &iosb, nullptr,
                                              0, FILE_SHARE_READ | FILE_SHARE_WRITE, kFileOpen,
                                              0, nullptr, 0);
    if (status < 0) throw_errno(ntstatus_to_errno(status), "Poller: open \\Device\\Afd");
    // Bound to the port before any poll is sent through it, so its polls are
    // thread-agnostic: they survive the exit of the thread that sent them.
    if (::CreateIoCompletionPort(handle, port, kAfdKey, 0) == nullptr ||
        !::SetFileCompletionNotificationModes(handle, FILE_SKIP_SET_EVENT_ON_HANDLE)) {
        const int err = win32_to_errno(::GetLastError());
        ::CloseHandle(handle);
        throw_errno(err, "Poller: bind \\Device\\Afd to the completion port");
    }
    auto afd    = std::make_unique<AfdHandle>();
    afd->handle = handle;
    afd->users  = 1;
    afds.push_back(std::move(afd));
    return afds.back().get();
}

void IocpPoller::release_afd(AfdHandle* afd) noexcept {
    std::lock_guard lock(afd_mutex);
    if (--afd->users != 0) return;
    // No SockState uses it, and each one's last poll has completed (a state is only
    // destroyed once the kernel has let go of it), so nothing is outstanding on it.
    ::CloseHandle(afd->handle);
    std::erase_if(afds, [afd](const auto& p) { return p.get() == afd; });
}

void IocpPoller::add_inflight() noexcept {
    std::lock_guard lock(inflight_mutex);
    ++inflight;
}

void IocpPoller::sub_inflight() noexcept {
    std::lock_guard lock(inflight_mutex);
    --inflight;
}

std::size_t IocpPoller::inflight_count() noexcept {
    std::lock_guard lock(inflight_mutex);
    return inflight;
}

std::shared_ptr<SockState> IocpPoller::find(RawFd fd) {
    std::lock_guard lock(map_mutex);
    const auto it = sockets.find(fd);
    return it == sockets.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Submitting and cancelling polls. Each is called with `s.mutex` held.
// ---------------------------------------------------------------------------

// Sends one IOCTL_AFD_POLL for `s.armed`. Requires status Idle. Returns 0, or the
// errno of a submission the kernel refused (no completion follows then).
int IocpPoller::submit(SockState& s) noexcept {
    s.poll_info.timeout.QuadPart     = INT64_MAX;
    s.poll_info.number_of_handles    = 1;
    s.poll_info.exclusive            = FALSE;
    s.poll_info.handles[0].handle    = reinterpret_cast<HANDLE>(s.base);
    s.poll_info.handles[0].events    = s.armed | kAfdPollLocalClose;
    s.poll_info.handles[0].status    = 0;
    s.iosb.Status = kStatusPending;

    // ApcContext becomes the completion packet's overlapped pointer.
    const NTSTATUS status = nt().NtDeviceIoControlFile(
        s.afd->handle, nullptr, nullptr, &s, &s.iosb, kIoctlAfdPoll, &s.poll_info,
        sizeof(s.poll_info), &s.poll_info, sizeof(s.poll_info));
    // STATUS_PENDING, or STATUS_SUCCESS when the socket was ready already: either way
    // a packet will be queued (the handle doesn't skip the port on success).
    if (status < 0) return ntstatus_to_errno(status);
    s.status    = PollStatus::Pending;
    s.submitted = s.armed;
    s.in_flight = s.shared_from_this();
    add_inflight();
    return 0;
}

// Asks the kernel to end the poll in flight. Its completion still arrives, with
// STATUS_CANCELLED, or with its events if it had completed already.
void IocpPoller::cancel(SockState& s) noexcept {
    IO_STATUS_BLOCK cancel_iosb{};
    // STATUS_NOT_FOUND: the poll had completed already and its packet is on its way.
    // Not read from s.iosb first: the kernel may be writing it.
    (void)nt().NtCancelIoFileEx(s.afd->handle, &s.iosb, &cancel_iosb);
    s.status    = PollStatus::Cancelled;
    s.submitted = 0;
}

// After `armed` changed on a thread other than the polling one: keeps one poll in
// flight that asks for exactly `armed`.
void IocpPoller::update(SockState& s) noexcept {
    if (s.deleted) return;
    switch (s.status) {
    case PollStatus::Idle:
        if (s.armed != 0 && submit(s) != 0) post_error(s);
        break;
    case PollStatus::Pending:
        // Never replaced in place: cancel, and poll() resubmits with the new mask when
        // the cancellation completes.
        if (s.submitted != s.armed) cancel(s);
        break;
    case PollStatus::Cancelled:
        break;   // poll() resubmits with the current mask
    }
}

// Reports a failed submission as an error event, from a thread that can't append to
// poll()'s output: a kErrorKey packet makes the next poll() report it.
void IocpPoller::post_error(SockState& s) noexcept {
    if (s.error_ref) return;   // one is already on its way
    s.error_ref = s.shared_from_this();
    add_inflight();
    if (!::PostQueuedCompletionStatus(port, 0, kErrorKey, reinterpret_cast<LPOVERLAPPED>(&s))) {
        // Nothing more can be done: the waiter's next syscall or timeout decides.
        s.error_ref.reset();
        sub_inflight();
    }
}

// ---------------------------------------------------------------------------
// Poller
// ---------------------------------------------------------------------------

Poller::Poller(PollerOptions) : m_impl(std::make_unique<IocpPoller>()) {
    winsock_init();
    m_impl->port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (m_impl->port == nullptr)
        throw_errno(win32_to_errno(::GetLastError()), "CreateIoCompletionPort");

    // The timeout timer. Without it (neither is expected to fail on Windows 10 1803 or
    // later), timed waits fall back to whole milliseconds, rounded up.
    m_impl->timer = ::CreateWaitableTimerExW(nullptr, nullptr,
                                             CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_ALL_ACCESS);
    if (m_impl->timer == nullptr)
        m_impl->timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (m_impl->timer != nullptr) {
        HANDLE packet = nullptr;
        if (nt().NtCreateWaitCompletionPacket(&packet, GENERIC_ALL, nullptr) >= 0)
            m_impl->wait_packet = packet;
    }
}

Poller::~Poller() {
    if (!m_impl) return;
    IocpPoller& m = *m_impl;

    // Every registration should be gone already (IoDriver's warning). Any left are
    // cancelled here, so the drain below can collect their last completions.
    std::unordered_map<RawFd, std::shared_ptr<SockState>> left;
    {
        std::lock_guard lock(m.map_mutex);
        left.swap(m.sockets);
    }
    for (auto& [fd, state] : left) {
        std::lock_guard lock(state->mutex);
        state->deleted = true;
        if (state->status == PollStatus::Pending) m.cancel(*state);
    }
    left.clear();
    m.deferred.clear();

    // The kernel writes each poll's IO_STATUS_BLOCK and AfdPollInfo inside its
    // SockState, so no state may be freed before its completion has been dequeued.
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (m.inflight_count() != 0) {
        ULONG n = 0;
        if (!::GetQueuedCompletionStatusEx(m.port, m.entries, kMaxEntriesPerWait, &n, 100,
                                           FALSE))
            n = 0;
        for (ULONG i = 0; i < n; ++i) {
            const OVERLAPPED_ENTRY& e = m.entries[i];
            if (e.lpCompletionKey != kAfdKey && e.lpCompletionKey != kErrorKey) continue;
            auto* s = reinterpret_cast<SockState*>(e.lpOverlapped);
            std::shared_ptr<SockState> keep;
            {
                std::lock_guard lock(s->mutex);
                keep = std::move(e.lpCompletionKey == kAfdKey ? s->in_flight : s->error_ref);
            }
            m.sub_inflight();
        }   // `keep` may free the state here
        if (std::chrono::steady_clock::now() > give_up) {
            // A poll that never completes: leak everything rather than free memory
            // the kernel may still write.
            (void)m_impl.release();
            return;
        }
    }

    if (m.wait_packet != nullptr) ::CloseHandle(m.wait_packet);
    if (m.timer != nullptr) ::CloseHandle(m.timer);
    {
        std::lock_guard lock(m.afd_mutex);
        for (auto& afd : m.afds) ::CloseHandle(afd->handle);   // none expected
        m.afds.clear();
    }
    ::CloseHandle(m.port);
}

void Poller::register_fd(RawFd fd, void* key, Interest interest, Trigger trigger) {
    IocpPoller& m = *m_impl;
    auto state      = std::make_shared<SockState>(m);
    state->fd       = fd;
    state->base     = base_socket(to_socket(fd));
    state->key      = key;
    state->interest = afd_bits(interest);
    state->armed    = state->interest;
    state->level    = trigger == Trigger::Level;
    state->afd      = m.acquire_afd();   // released by ~SockState

    {
        std::lock_guard lock(m.map_mutex);
        if (!m.sockets.emplace(fd, state).second)
            throw_errno(EEXIST, "Poller::register_fd");
    }
    // Race (handled): poll() on another thread can dequeue this poll's completion as
    // soon as it is submitted; it takes the state's mutex first, so it sees the
    // state complete.
    std::lock_guard lock(state->mutex);
    if (const int err = m.submit(*state); err != 0) {
        {
            std::lock_guard map_lock(m.map_mutex);
            m.sockets.erase(fd);
        }
        throw_errno(err, "Poller::register_fd");
    }
}

void Poller::reregister_fd(RawFd fd, void* key, Interest interest, Trigger trigger) {
    IocpPoller& m = *m_impl;
    auto state = m.find(fd);
    if (!state) throw_errno(ENOENT, "Poller::reregister_fd");
    std::lock_guard lock(state->mutex);
    state->key      = key;
    state->interest = afd_bits(interest);
    state->armed    = state->interest;
    state->level    = trigger == Trigger::Level;
    // A poll in flight for a different mask is cancelled; a completion already in the
    // port is reported against the new mask (poll() masks with `armed`).
    m.update(*state);
}

bool Poller::deregister_fd(RawFd fd) noexcept {
    IocpPoller& m = *m_impl;
    std::shared_ptr<SockState> state;
    {
        std::lock_guard lock(m.map_mutex);
        const auto it = m.sockets.find(fd);
        if (it == m.sockets.end()) return false;
        state = std::move(it->second);
        m.sockets.erase(it);
    }
    std::lock_guard lock(state->mutex);
    // From here no poll() reports this key. A poll() that built an event for it just
    // before is covered by IoDriver's pending release, as on epoll.
    state->deleted = true;
    if (state->status == PollStatus::Pending) m.cancel(*state);
    // The state lives on, through `in_flight`, until the cancelled poll's completion
    // has been dequeued.
    return true;
}

void Poller::rearm(RawFd fd, Interest interest) noexcept {
    IocpPoller& m = *m_impl;
    std::shared_ptr<SockState> state;
    try {
        state = m.find(fd);
    } catch (...) {
        return;   // only a failed lock can throw here
    }
    if (!state) return;
    std::lock_guard lock(state->mutex);
    if (state->deleted || state->level) return;   // level-triggered never disarms
    const ULONG before = state->armed;
    state->armed |= afd_bits(interest) & state->interest;
    if (state->armed != before) m.update(*state);
}

namespace {

// What one poll() call is building: the events it appends, one per state, merged.
struct Batch {
    IocpPoller&       m;
    std::vector<Event>& out;
    bool                resubmitted = false;   // a poll was resubmitted in this pass
    bool                woke        = false;   // a PollWaker packet was seen
    bool                timer_fired = false;   // the timeout timer's packet was seen
};

void add_event(Batch& b, SockState& s, bool readable, bool writable, bool error, bool hup) {
    if (s.event_gen == b.m.generation) {
        Event& e = b.out[s.event_index];
        e.key       = s.key;
        e.readable  = e.readable || readable;
        e.writable  = e.writable || writable;
        e.error     = e.error || error;
        e.hup       = e.hup || hup;
        return;
    }
    s.event_gen   = b.m.generation;
    s.event_index = b.out.size();
    b.out.push_back(Event{s.key, readable, writable, error, hup});
}

void add_afd_event(Batch& b, SockState& s, ULONG bits) {
    add_event(b, s,
              (bits & kReadBits) != 0,
              (bits & kWriteBits) != 0,
              (bits & (kAfdPollAbort | kAfdPollConnectFail)) != 0,
              (bits & (kAfdPollDisconnect | kAfdPollAbort)) != 0);
}

// Resubmits an Idle state for `armed`, from the polling thread. A level-triggered state
// is resubmitted once per poll(): its second completion in the same poll() waits for
// the next one, so that a socket that stays ready is reported once per poll().
void resubmit(Batch& b, SockState& s) {
    if (s.deleted || s.status != PollStatus::Idle || s.armed == 0) return;
    if (s.level) {
        if (s.resubmit_gen == b.m.generation) {
            if (!s.deferred) {
                s.deferred = true;
                b.m.deferred.push_back(s.shared_from_this());
            }
            return;
        }
        s.resubmit_gen = b.m.generation;
    }
    if (b.m.submit(s) == 0) {
        b.resubmitted = true;
    } else {
        add_event(b, s, true, true, true, false);   // the waiter's syscall reports it
        if (!s.level) s.armed = 0;
    }
}

void handle_afd(Batch& b, SockState* raw) {
    std::shared_ptr<SockState> keep;   // the kernel's reference, now ours
    bool local_close = false;
    {
        std::lock_guard lock(raw->mutex);
        keep = std::move(raw->in_flight);
        raw->status    = PollStatus::Idle;
        raw->submitted = 0;
        if (!raw->deleted) {
            const NTSTATUS status = raw->iosb.Status;
            ULONG events = 0;
            bool  failed = false;
            if (status == kStatusCancelled) {
                // Cancelled by rearm(), reregister_fd() or deregister_fd(): nothing to
                // report. Resubmitted below with the current mask.
            } else if (status < 0) {
                failed = true;
            } else if (raw->poll_info.number_of_handles >= 1) {
                events = raw->poll_info.handles[0].events;
            }

            if (events & kAfdPollLocalClose) {
                // Closed before it was deregistered: a broken IoRegistration contract.
                // Nothing can be polled any more.
                raw->deleted = true;
                local_close  = true;
            } else if (failed) {
                add_event(b, *raw, true, true, true, false);
                if (!raw->level) raw->armed = 0;   // re-armed by the waiter's next EAGAIN
            } else {
                const ULONG reported = events & raw->armed;
                if (reported != 0) {
                    add_afd_event(b, *raw, reported);
                    if (!raw->level) raw->armed &= ~reported;
                }
            }
            resubmit(b, *raw);
        }
    }
    b.m.sub_inflight();
    if (local_close) {
        std::lock_guard lock(b.m.map_mutex);
        const auto it = b.m.sockets.find(raw->fd);
        if (it != b.m.sockets.end() && it->second.get() == raw) b.m.sockets.erase(it);
    }
}   // `keep` may free the state here

void handle_error(Batch& b, SockState* raw) {
    std::shared_ptr<SockState> keep;
    {
        std::lock_guard lock(raw->mutex);
        keep = std::move(raw->error_ref);
        if (!raw->deleted) add_event(b, *raw, true, true, true, false);
    }
    b.m.sub_inflight();
}

void handle_entries(Batch& b, ULONG count) {
    for (ULONG i = 0; i < count; ++i) {
        const OVERLAPPED_ENTRY& e = b.m.entries[i];
        switch (e.lpCompletionKey) {
        case kWakeKey:
            b.out.push_back(Event{e.lpOverlapped, true, false, false, false});
            b.woke = true;
            break;
        case kTimerKey:
            // Also a stale packet from an earlier poll() whose cancel lost the race
            // with the expiry: ignored, at the cost of an empty pass.
            b.timer_fired = true;
            break;
        case kAfdKey:
            handle_afd(b, reinterpret_cast<SockState*>(e.lpOverlapped));
            break;
        case kErrorKey:
            handle_error(b, reinterpret_cast<SockState*>(e.lpOverlapped));
            break;
        default:
            break;
        }
    }
}

// One GetQueuedCompletionStatusEx(); returns the number of entries.
ULONG dequeue(IocpPoller& m, DWORD ms) {
    ULONG n = 0;
    if (::GetQueuedCompletionStatusEx(m.port, m.entries, kMaxEntriesPerWait, &n, ms, FALSE))
        return n;
    const DWORD err = ::GetLastError();
    if (err == WAIT_TIMEOUT) return 0;
    throw_errno(win32_to_errno(err), "GetQueuedCompletionStatusEx");
}

// Arms the timeout timer to post kTimerKey after `wait`. False if it can't be, and the
// wait must rely on its millisecond timeout alone.
bool arm_timer(IocpPoller& m, std::chrono::nanoseconds wait) noexcept {
    if (m.timer == nullptr || m.wait_packet == nullptr) return false;
    LARGE_INTEGER due;
    // Negative: relative, in 100 ns units, rounded up so the wait is never short.
    due.QuadPart = -std::max<LONGLONG>(1, (wait.count() + 99) / 100);
    if (!::SetWaitableTimer(m.timer, &due, 0, nullptr, nullptr, FALSE)) return false;
    BOOLEAN already_signaled = FALSE;
    // Fails if the packet is still queued from an earlier wait whose cancel lost the
    // race with the expiry. That wait then falls back to milliseconds.
    if (nt().NtAssociateWaitCompletionPacket(m.wait_packet, m.port, m.timer,
                                             reinterpret_cast<PVOID>(kTimerKey), nullptr,
                                             kStatusSuccess, 0, &already_signaled) < 0) {
        ::CancelWaitableTimer(m.timer);
        return false;
    }
    return true;
}

void disarm_timer(IocpPoller& m, bool fired) noexcept {
    // Removes the packet whether it is still waiting on the timer or already queued.
    // If it slips through anyway, a later poll() dequeues and ignores it.
    if (!fired) (void)nt().NtCancelWaitCompletionPacket(m.wait_packet, TRUE);
    ::CancelWaitableTimer(m.timer);
}

} // namespace

void Poller::poll(std::vector<Event>& out, std::optional<std::chrono::nanoseconds> timeout) {
    using SteadyClock = std::chrono::steady_clock;
    IocpPoller& m = *m_impl;
    ++m.generation;
    Batch b{m, out};
    const std::size_t first = out.size();

    // Level-triggered polls held back by the previous poll(): see resubmit().
    {
        auto deferred = std::move(m.deferred);
        m.deferred.clear();
        for (auto& s : deferred) {
            std::lock_guard lock(s->mutex);
            s->deferred = false;
            resubmit(b, *s);
        }
    }

    const bool zero = timeout && timeout->count() <= 0;
    std::optional<SteadyClock::time_point> deadline;
    if (timeout && !zero) deadline = SteadyClock::now() + *timeout;

    for (;;) {
        DWORD ms          = INFINITE;
        bool  timer_armed = false;
        if (zero) {
            ms = 0;
        } else if (deadline) {
            const auto remaining = *deadline - SteadyClock::now();
            if (remaining <= SteadyClock::duration::zero()) {
                ms = 0;
            } else {
                const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining);
                timer_armed = arm_timer(m, wait);
                // With the timer armed this only bounds the wait should its packet
                // never come; the timer ends it first.
                ms = ceil_ms(wait) + (timer_armed ? 1 : 0);
            }
        }

        b.timer_fired = false;
        ULONG n;
        try {
            n = dequeue(m, ms);
        } catch (...) {
            if (timer_armed) disarm_timer(m, false);
            throw;
        }
        handle_entries(b, n);
        if (timer_armed) disarm_timer(m, b.timer_fired);

        // Polls resubmitted for sockets that are ready already have completed by
        // now: report them in this poll() too.
        for (int pass = 0; pass < kMaxCollectPasses && b.resubmitted; ++pass) {
            b.resubmitted = false;
            handle_entries(b, dequeue(m, 0));
        }

        // Done once something was reported, or the wait is over. A wake by packets
        // that report nothing (cancellations, a stale timer) waits again for the rest
        // of the timeout, as epoll would not have woken at all.
        if (out.size() > first || b.woke || zero) return;
        if (deadline && SteadyClock::now() >= *deadline) return;
    }
}

// ---------------------------------------------------------------------------
// PollWaker
// ---------------------------------------------------------------------------

PollWaker::PollWaker(Poller& poller, void* key) : m_poller(poller), m_key(key) {}

// A wake packet still in the port is dequeued and ignored by ~Poller().
PollWaker::~PollWaker() = default;

void PollWaker::wake() noexcept {
    std::lock_guard lock(m_mutex);
    // At most one packet in the port: repeated wakes before a poll() don't pile up.
    if (m_posted) return;
    if (::PostQueuedCompletionStatus(m_poller.m_impl->port, 0, kWakeKey,
                                     static_cast<LPOVERLAPPED>(m_key)))
        m_posted = true;
}

void PollWaker::reset() noexcept {
    // Race (benign): a wake() between poll() dequeuing the packet and this call is
    // absorbed, as on the eventfd. The caller is the polling thread, already awake,
    // and re-checks its queues before it polls again.
    std::lock_guard lock(m_mutex);
    m_posted = false;
}

} // namespace coro::detail::sys
