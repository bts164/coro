#pragma once

/* ---------------------------------------------------------------------------
 * lwIP options for host-side NO_SYS testing.
 *
 * Used when building the lwIP backend tests on Linux. A PicoNetwork::Lwip Runtime
 * runs lwIP's timers on its (stub) async_context, and the lwIP Poller drains the
 * loopback queue. Only loopback networking is needed — no ARP, no real netif
 * drivers.
 * ------------------------------------------------------------------------- */

/* NO_SYS — no RTOS, no OS primitives */
#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0
#define LWIP_NETCONN            0
#define LWIP_SOCKET             0

/* Protocols */
#define LWIP_TCP                1
#define LWIP_UDP                1   /* required by the DNS module; also backs UdpSocket */
#define LWIP_ICMP               0
#define LWIP_ARP                0
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_RAW                0

/* IGMP — needed for UdpSocket::join_multicast()/leave_multicast() tests */
#define LWIP_IGMP               1

/* DNS — needed for TcpStream::connect() hostname resolution */
#define LWIP_DNS                1
#define DNS_TABLE_SIZE          4
#define DNS_MAX_NAME_LENGTH     256

/* MQTT — needed for MqttClient (src/pico/mqtt.cpp). Uses the raw TCP API
 * directly (LWIP_ALTCP left unset), same as the rest of this test config. */
#define LWIP_MQTT                1

/* Loopback netif */
#define LWIP_HAVE_LOOPIF        1
#define LWIP_NETIF_LOOPBACK     1

/* Required by coro's lwIP sys backend, as in src/detail/sys/lwip/lwipopts.h.in */
#define SO_REUSE                1
#define TCP_LISTEN_BACKLOG      1

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

/* Stats — off for test builds */
#define LWIP_STATS              0

/* Debug — set LWIP_DEBUG to 1 to enable lwIP diagnostic output */
#define LWIP_DEBUG              0
