// lwIP backend for the TCP seam (include/coro/detail/sys/tcp.h), over the raw TCP API.
// See doc/design/pico_port.md, "TCP" and "Received data: per-socket buffers".
//
// lwIP's callbacks run on the executor's thread inside Poller::poll(). They only
// update their LwipTcp/LwipListener and mark it ready; the try_ functions are the
// "syscalls" that TcpStream's and TcpListener's futures make.

#include "io_object.h"

#include <coro/detail/sys/socket.h>
#include <coro/detail/sys/tcp.h>

#include <lwip/opt.h>
#include <lwip/tcp.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace coro::detail::sys {

namespace {

// How often lwIP calls the poll callback, in units of its coarse timer (500 ms):
// about once a second. It catches a write that saw ERR_MEM with nothing in flight,
// where no `sent` callback would ever come.
constexpr u8_t kPollInterval = 2;

// The receive ring holds what lwIP's receive window lets the peer send ahead of the
// reader: TCP_WND bytes.
constexpr std::size_t kReceiveRing = static_cast<std::size_t>(TCP_WND);

err_t on_recv(void* arg, tcp_pcb* /*pcb*/, pbuf* p, err_t err) {
    auto* tcp = static_cast<LwipTcp*>(arg);
    if (p == nullptr) {
        // The peer closed its side. Reads return what is buffered, then 0.
        tcp->eof = true;
        mark_ready(tcp, IoObject::kReadable | IoObject::kHup);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        tcp->error = lwip_errno(err);
        mark_ready(tcp, IoObject::kReadable | IoObject::kError);
        return ERR_OK;
    }
    if (!tcp->rx) {
        // The first data. An exception must not unwind into lwIP's C code, so a
        // failed allocation refuses the segment: lwIP keeps it as the pcb's refused
        // data and offers it again on a later timer tick.
        try {
            tcp->rx.emplace(kReceiveRing);
        } catch (...) {
            return ERR_MEM;
        }
    }
    if (p->tot_len > tcp->rx->writable_bytes()) {
        // Cannot happen while the peer honours the window: bytes in the ring plus
        // those in flight never exceed TCP_WND, because only reads reopen it.
        // Refused rather than dropped, as above.
        return ERR_MEM;
    }
    ring_write(*tcp->rx, p);
    // Freed now, so unread data never holds the radio's receive pool. The window
    // reopens only when a read takes the bytes (tcp_try_read()).
    pbuf_free(p);
    mark_ready(tcp, IoObject::kReadable);
    return ERR_OK;
}

err_t on_sent(void* arg, tcp_pcb* /*pcb*/, u16_t /*len*/) {
    auto* tcp = static_cast<LwipTcp*>(arg);
    tcp->write_waiting = false;
    mark_ready(tcp, IoObject::kWritable);
    return ERR_OK;
}

err_t on_poll(void* arg, tcp_pcb* pcb) {
    auto* tcp = static_cast<LwipTcp*>(arg);
    if (tcp->write_waiting && tcp_sndbuf(pcb) > 0) {
        tcp->write_waiting = false;
        mark_ready(tcp, IoObject::kWritable);
    }
    return ERR_OK;
}

// lwIP has freed the pcb before calling this.
void on_err(void* arg, err_t err) {
    auto* tcp = static_cast<LwipTcp*>(arg);
    tcp->pcb = nullptr;
    int e = lwip_errno(err);
    // A reset while connecting is a refused connection, as on POSIX.
    if (tcp->connecting && err == ERR_RST) e = ECONNREFUSED;
    if (e == 0) e = ECONNRESET;
    tcp->error      = e;
    tcp->connecting = false;
    mark_ready(tcp, IoObject::kReadable | IoObject::kWritable | IoObject::kError);
}

err_t on_connected(void* arg, tcp_pcb* /*pcb*/, err_t err) {
    auto* tcp = static_cast<LwipTcp*>(arg);
    tcp->connecting = false;
    if (err != ERR_OK) {
        tcp->error = lwip_errno(err);
        mark_ready(tcp, IoObject::kWritable | IoObject::kError);
        return ERR_OK;
    }
    tcp->connected = true;
    mark_ready(tcp, IoObject::kWritable);
    return ERR_OK;
}

void install_callbacks(LwipTcp* tcp, tcp_pcb* pcb) {
    tcp_arg (pcb, tcp);
    tcp_recv(pcb, &on_recv);
    tcp_sent(pcb, &on_sent);
    tcp_err (pcb, &on_err);
    tcp_poll(pcb, &on_poll, kPollInterval);
    // Every write is flushed at once (tcp_try_write() calls tcp_output()).
    tcp_nagle_disable(pcb);
}

void detach_callbacks(tcp_pcb* pcb) {
    tcp_arg (pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_err (pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
}

err_t on_accept(void* arg, tcp_pcb* newpcb, err_t err) {
    auto* listener = static_cast<LwipListener*>(arg);
    if (err != ERR_OK || newpcb == nullptr) return ERR_VAL;
    // No exception may unwind into lwIP: allocation failures refuse the connection,
    // which lwIP then aborts.
    auto* tcp = new (std::nothrow) LwipTcp();
    if (tcp == nullptr) return ERR_MEM;
    tcp->pcb       = newpcb;
    tcp->connected = true;
    // Installed now, not at accept(): data can arrive in the same poll that accepted
    // the connection, and is buffered rather than lost.
    install_callbacks(tcp, newpcb);
    try {
        listener->queue.push_back(tcp);
    } catch (...) {
        detach_callbacks(newpcb);
        delete tcp;
        return ERR_MEM;
    }
#if TCP_LISTEN_BACKLOG
    // Counts against the listener's backlog until tcp_try_accept() hands it out.
    tcp_backlog_delayed(newpcb);
#endif
    mark_ready(listener, IoObject::kReadable);
    return ERR_OK;
}

// tcp_recved() takes a 16-bit count.
void reopen_window(tcp_pcb* pcb, std::size_t n) {
    while (n > 0) {
        const auto chunk = static_cast<u16_t>(std::min<std::size_t>(n, 0xFFFF));
        tcp_recved(pcb, chunk);
        n -= chunk;
    }
}

} // namespace

RawFd tcp_connect_start(const SocketAddress& peer) {
    require_lwip("TcpStream::connect");
    ip_addr_t addr;
    if (!to_ip_addr(peer, addr)) throw_errno(EAFNOSUPPORT, "TcpStream::connect");

    auto tcp = std::make_unique<LwipTcp>();
    tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == nullptr) throw_errno(ENOMEM, "TcpStream::connect");
    tcp->pcb        = pcb;
    tcp->connecting = true;
    install_callbacks(tcp.get(), pcb);
    const err_t err = tcp_connect(pcb, &addr, peer.port, &on_connected);
    if (err != ERR_OK) {
        detach_callbacks(pcb);
        tcp_abort(pcb);
        throw_errno(lwip_errno(err), "TcpStream::connect");
    }
    return tcp.release();
}

IoResult tcp_try_finish_connect(RawFd fd) noexcept {
    auto* tcp = static_cast<LwipTcp*>(fd);
    if (tcp->connected) return 0;
    if (tcp->error != 0) return std::unexpected(tcp->error);
    return std::unexpected(EAGAIN);
}

RawFd tcp_listen(const SocketAddress& local, int backlog) {
    require_lwip("TcpListener::bind");
    ip_addr_t addr;
    if (!to_ip_addr(local, addr)) throw_errno(EAFNOSUPPORT, "TcpListener::bind");

    auto listener = std::make_unique<LwipListener>();
    tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == nullptr) throw_errno(ENOMEM, "TcpListener::bind");
#if SO_REUSE
    // As SO_REUSEADDR on POSIX: a restarted server can rebind its port.
    ip_set_option(pcb, SOF_REUSEADDR);
#endif
    const err_t bind_err = tcp_bind(pcb, &addr, local.port);
    if (bind_err != ERR_OK) {
        tcp_close(pcb);   // still CLOSED: frees it at once
        throw_errno(lwip_errno(bind_err), "TcpListener::bind");
    }
    // Every pending connection holds a pcb, and the backlog is 8 bits.
    const int capped = std::clamp(backlog, 1, std::min(255, static_cast<int>(MEMP_NUM_TCP_PCB)));
    tcp_pcb* listen_pcb = tcp_listen_with_backlog(pcb, static_cast<u8_t>(capped));
    if (listen_pcb == nullptr) {
        tcp_close(pcb);   // not freed by a failed tcp_listen
        throw_errno(ENOMEM, "TcpListener::bind");
    }
    listener->pcb = listen_pcb;
    tcp_arg(listen_pcb, listener.get());
    tcp_accept(listen_pcb, &on_accept);
    return listener.release();
}

std::expected<RawFd, int> tcp_try_accept(RawFd fd) noexcept {
    auto* listener = static_cast<LwipListener*>(fd);
    while (!listener->queue.empty()) {
        LwipTcp* tcp = listener->queue.front();
        listener->queue.pop_front();
        if (tcp->pcb == nullptr) {
            // Failed while queued: skipped, as accept4()'s ECONNABORTED is on POSIX.
            close_socket(tcp);
            continue;
        }
#if TCP_LISTEN_BACKLOG
        tcp_backlog_accepted(tcp->pcb);
#endif
        return tcp;
    }
    return std::unexpected(EAGAIN);
}

IoResult tcp_try_read(RawFd fd, std::byte* data, std::size_t size) noexcept {
    auto* tcp = static_cast<LwipTcp*>(fd);
    if (size == 0) return 0;
    const std::size_t buffered = tcp->rx ? tcp->rx->readable_bytes() : 0;
    if (buffered > 0) {
        // Bytes the peer sent before an error or a close are delivered first.
        const std::size_t n = std::min(size, buffered);
        ring_read(*tcp->rx, data, n);
        // Only now does the peer get the room back: TCP's flow control.
        if (tcp->pcb != nullptr) reopen_window(tcp->pcb, n);
        return n;
    }
    if (tcp->error != 0) return std::unexpected(tcp->error);
    if (tcp->eof) return 0;
    if (tcp->pcb == nullptr) return std::unexpected(ENOTCONN);
    return std::unexpected(EAGAIN);
}

IoResult tcp_try_write(RawFd fd, const std::byte* data, std::size_t size) noexcept {
    auto* tcp = static_cast<LwipTcp*>(fd);
    if (tcp->error != 0) return std::unexpected(tcp->error);
    if (tcp->pcb == nullptr) return std::unexpected(EPIPE);
    if (size == 0) return 0;
    const u16_t room = tcp_sndbuf(tcp->pcb);
    if (room == 0) {
        // Data is in flight, so `sent` will report room. Flush first, in case it
        // is still queued.
        tcp_output(tcp->pcb);
        tcp->write_waiting = true;
        return std::unexpected(EAGAIN);
    }
    const auto n = static_cast<u16_t>(std::min<std::size_t>(size, room));
    // TCP_WRITE_FLAG_COPY: lwIP copies the bytes now, so the caller's buffer is
    // never lent across a suspension.
    const err_t err = tcp_write(tcp->pcb, data, n, TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM) {
        // Out of segments or pbufs. If nothing is in flight no `sent` follows; the
        // poll callback then reports room (see on_poll()).
        tcp_output(tcp->pcb);
        tcp->write_waiting = true;
        return std::unexpected(EAGAIN);
    }
    if (err != ERR_OK) return std::unexpected(err == ERR_CONN ? EPIPE : lwip_errno(err));
    tcp_output(tcp->pcb);
    return static_cast<std::size_t>(n);
}

void close_tcp(LwipTcp* tcp) noexcept {
    if (tcp->pcb != nullptr) {
        detach_callbacks(tcp->pcb);
        // A FIN, so the peer gets data already written. lwIP sends a reset instead
        // when received data is discarded unread, as Linux does. tcp_close() fails
        // only for lack of memory; abort then.
        if (tcp_close(tcp->pcb) != ERR_OK) tcp_abort(tcp->pcb);
        tcp->pcb = nullptr;
    }
    delete tcp;
}

void close_listener(LwipListener* listener) noexcept {
    // Connections never accepted are reset, as a kernel resets its backlog.
    for (LwipTcp* tcp : listener->queue) {
        if (tcp->pcb != nullptr) {
            detach_callbacks(tcp->pcb);
            tcp_abort(tcp->pcb);
        }
        delete tcp;
    }
    listener->queue.clear();
    if (listener->pcb != nullptr) {
        tcp_arg(listener->pcb, nullptr);
        tcp_accept(listener->pcb, nullptr);
        tcp_close(listener->pcb);
    }
    delete listener;
}

} // namespace coro::detail::sys
