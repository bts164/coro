#pragma once
// TestNetif: a second lwIP interface for the socket tests, beside loopback, that
// stands in for a link. It hands every frame the stack sends back to the stack with
// its source and destination addresses swapped, so a connection to kPeer arrives at
// kLocal, from kPeer. Loopback delivers in the same service pass; this delivers from
// an lwIP timer, so the runtime goes to sleep between a send and its arrival, as it
// does with a radio. A policy decides each frame's fate: deliver it after a delay,
// drop it, or refuse it as a driver out of buffers does.
//
// Swapping the addresses leaves every checksum valid: the IP header's and the
// pseudo-header's both sum the two addresses, in either order.
//
// lwIP only. Not thread-safe: make it, set its policy and drop it on the runtime's
// thread, as with everything that touches lwIP. Drop it before the runtime, which
// stops lwIP.

// First: the SDK's arch/cc.h defines sys_prot_t only if NO_SYS, which it can see
// only once opt.h has read lwipopts.h.
#include <lwip/opt.h>
#include <lwip/def.h>
#include <lwip/ip.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/prot/ip.h>
#include <lwip/prot/ip4.h>
#include <lwip/prot/tcp.h>
#include <lwip/prot/udp.h>
#include <lwip/sys.h>   // sys_now()
#include <lwip/timeouts.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

class TestNetif {
public:
    static constexpr const char* kLocal = "10.99.0.1";   // the netif's own address
    static constexpr const char* kPeer  = "10.99.0.2";   // send here to reach kLocal

    // A frame as the stack sent it, before the swap.
    struct Frame {
        uint8_t     protocol  = 0;   // IP_PROTO_TCP, IP_PROTO_UDP, ...
        uint16_t    src_port  = 0;   // TCP and UDP
        uint16_t    dst_port  = 0;
        uint8_t     tcp_flags = 0;   // TCP_SYN, TCP_FIN, ...
        uint16_t    window    = 0;   // the receive window a TCP frame advertises
        std::size_t payload   = 0;   // bytes after the TCP or UDP header
    };

    struct Verdict {
        enum class Action { deliver, drop, refuse };
        Action                    action = Action::deliver;
        std::chrono::milliseconds delay{0};   // deliver only

        static Verdict deliver(std::chrono::milliseconds delay = {}) {
            return {Action::deliver, delay};
        }
        static Verdict drop() { return {Action::drop, {}}; }
        // The netif's output fails with ERR_MEM, and the frame goes nowhere.
        static Verdict refuse() { return {Action::refuse, {}}; }
    };

    // Called once per frame, from inside lwIP: it must not call into lwIP.
    using Policy = std::function<Verdict(const Frame&)>;

    TestNetif() {
        ip4_addr_t addr{};
        ip4_addr_t mask{};
        ip4_addr_t gw{};
        ip4addr_aton(kLocal, &addr);
        IP4_ADDR(&mask, 255, 255, 255, 0);
        if (netif_add(&m_netif, &addr, &mask, &gw, this, &TestNetif::init, ip4_input) == nullptr)
            throw std::runtime_error("TestNetif: netif_add failed");
        netif_set_link_up(&m_netif);
        netif_set_up(&m_netif);
    }

    ~TestNetif() {
        // netif_remove() aborts the pcbs still bound to kLocal, and their RSTs come
        // through output(): with no policy they are queued, then freed below.
        m_policy = nullptr;
        netif_remove(&m_netif);
        sys_untimeout(&TestNetif::on_timer, this);
        for (const Queued& q : m_queue) pbuf_free(q.p);
    }

    TestNetif(const TestNetif&)            = delete;   // lwIP holds &m_netif
    TestNetif& operator=(const TestNetif&) = delete;

    // Applies to the frames sent from now on; those already queued keep their time.
    // With no policy, every frame is delivered with no delay.
    void set_policy(Policy policy) { m_policy = std::move(policy); }

    // Frames by fate so far. Delivered counts those queued, arrived or not.
    int delivered() const { return m_delivered; }
    int dropped() const { return m_dropped; }
    int refused() const { return m_refused; }

private:
    struct Queued {
        u32_t due;   // sys_now() time
        pbuf* p;
    };

    // True if lwIP time a comes before b, across the 32-bit wrap.
    static bool before(u32_t a, u32_t b) { return static_cast<int32_t>(a - b) < 0; }

    static err_t init(netif* n) {
        n->name[0] = 't';
        n->name[1] = 'n';
        n->output  = &TestNetif::output;
        n->mtu     = 1500;
        return ERR_OK;
    }

    static err_t output(netif* n, pbuf* p, const ip4_addr_t* /*next_hop*/) {
        return static_cast<TestNetif*>(n->state)->send(p);
    }

    static void on_timer(void* arg) { static_cast<TestNetif*>(arg)->deliver_due(); }

    // lwIP keeps `p`, a TCP segment until it is acknowledged, so the frame is a copy:
    // one contiguous pbuf, which parse() can read as a header.
    err_t send(pbuf* p) {
        pbuf* copy = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
        if (copy == nullptr) return ERR_MEM;
        const Verdict verdict = m_policy ? m_policy(parse(*copy)) : Verdict::deliver();
        switch (verdict.action) {
        case Verdict::Action::refuse:
            pbuf_free(copy);
            ++m_refused;
            return ERR_MEM;
        case Verdict::Action::drop:
            pbuf_free(copy);
            ++m_dropped;
            return ERR_OK;   // as a link loses it: sent, never arrives
        case Verdict::Action::deliver:
            break;
        }
        auto* ip = static_cast<ip_hdr*>(copy->payload);
        const ip4_addr_p_t src = ip->src;
        ip->src  = ip->dest;
        ip->dest = src;
        // Frames due at the same time keep their order: TCP sees no reordering the
        // policy didn't ask for.
        const Queued q{sys_now() + static_cast<u32_t>(verdict.delay.count()), copy};
        auto at = std::upper_bound(m_queue.begin(), m_queue.end(), q,
                                   [](const Queued& a, const Queued& b) { return before(a.due, b.due); });
        m_queue.insert(at, q);
        ++m_delivered;
        schedule();
        return ERR_OK;
    }

    // Always from the timer, never from send(): input there would re-enter the TCP
    // code that is sending.
    void deliver_due() {
        const u32_t now = sys_now();
        while (!m_queue.empty() && !before(now, m_queue.front().due)) {
            pbuf* p = m_queue.front().p;
            m_queue.erase(m_queue.begin());   // first: the input may send, and queue more
            if (m_netif.input(p, &m_netif) != ERR_OK) pbuf_free(p);
        }
        schedule();
    }

    // One lwIP timeout, for the earliest frame: the board has MEMP_NUM_SYS_TIMEOUT
    // of them, shared with lwIP's own timers.
    void schedule() {
        sys_untimeout(&TestNetif::on_timer, this);
        if (m_queue.empty()) return;
        const auto wait = static_cast<int32_t>(m_queue.front().due - sys_now());
        sys_timeout(wait > 0 ? static_cast<u32_t>(wait) : 0, &TestNetif::on_timer, this);
    }

    static Frame parse(const pbuf& p) {
        const auto* bytes = static_cast<const uint8_t*>(p.payload);
        const auto* ip    = reinterpret_cast<const ip_hdr*>(bytes);
        const std::size_t header = IPH_HL_BYTES(ip);
        const std::size_t total  = lwip_ntohs(IPH_LEN(ip));
        Frame frame;
        frame.protocol = IPH_PROTO(ip);
        if (frame.protocol == IP_PROTO_TCP) {
            const auto* tcp = reinterpret_cast<const tcp_hdr*>(bytes + header);
            frame.src_port  = lwip_ntohs(tcp->src);
            frame.dst_port  = lwip_ntohs(tcp->dest);
            frame.tcp_flags = TCPH_FLAGS(tcp);
            frame.window    = lwip_ntohs(tcp->wnd);
            frame.payload   = total - header - TCPH_HDRLEN_BYTES(tcp);
        } else if (frame.protocol == IP_PROTO_UDP) {
            const auto* udp = reinterpret_cast<const udp_hdr*>(bytes + header);
            frame.src_port  = lwip_ntohs(udp->src);
            frame.dst_port  = lwip_ntohs(udp->dest);
            frame.payload   = total - header - UDP_HLEN;
        }
        return frame;
    }

    netif               m_netif{};
    Policy              m_policy;
    std::vector<Queued> m_queue;   // by due time
    int                 m_delivered = 0;
    int                 m_dropped   = 0;
    int                 m_refused   = 0;
};
