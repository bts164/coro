#pragma once

/* ---------------------------------------------------------------------------
 * lwIP options for host-side NO_SYS testing.
 *
 * Used when building the lwIP TCP backend tests on Linux. The application
 * drives the poll loop manually: rt.poll() + sys_check_timeouts() + netif_poll_all().
 * Only loopback networking is needed — no ARP, no real netif drivers.
 * ------------------------------------------------------------------------- */

/* NO_SYS — no RTOS, no OS primitives */
#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0
#define LWIP_NETCONN            0
#define LWIP_SOCKET             0

/* Protocols — TCP only, no UDP/ICMP/ARP needed for loopback tests */
#define LWIP_TCP                1
#define LWIP_UDP                1   /* required by the DNS module */
#define LWIP_ICMP               0
#define LWIP_ARP                0
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_RAW                0

/* DNS — needed for TcpStream::connect() hostname resolution */
#define LWIP_DNS                1
#define DNS_TABLE_SIZE          4
#define DNS_MAX_NAME_LENGTH     256

/* Loopback netif */
#define LWIP_HAVE_LOOPIF        1
#define LWIP_NETIF_LOOPBACK     1

/* Memory — use system heap; avoids having to tune pool sizes */
#define MEM_LIBC_MALLOC         1
#define MEMP_MEM_MALLOC         1
#define MEM_ALIGNMENT           4
#define MEM_SIZE                (32 * 1024)

/* TCP buffer sizes */
#define TCP_MSS                 1460
#define TCP_SND_BUF             (4 * TCP_MSS)
#define TCP_WND                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN        (4 * TCP_SND_BUF / TCP_MSS)

/* Random number source — used by DNS for transaction ID generation */
#define LWIP_RAND()             ((u32_t)rand())

/* Timer intervals — reduced from defaults (250ms / 500ms) so that delayed ACKs
 * and retransmission timers fire quickly during the tight poll loop in unit
 * tests. sys_now() returns real milliseconds, so these control actual wall-clock
 * delay between sys_check_timeouts() calls triggering the callbacks. */
#define TCP_FAST_INTERVAL       1   /* ms — sends delayed ACKs; default 250ms */
#define TCP_SLOW_INTERVAL       2   /* ms — retransmissions etc; default 500ms */

/* Stats — off for test builds */
#define LWIP_STATS              0

/* Debug — set LWIP_DEBUG to 1 to enable lwIP diagnostic output */
#define LWIP_DEBUG              0
