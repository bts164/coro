#pragma once

// lwIP options for Pico W (pico_cyw43_arch_lwip_poll mode).
//
// Derived from the reference configuration in pico-examples:
//   pico_w/wifi/lwipopts_examples_common.h
//
// This file must be on the include path for every target that links
// pico_cyw43_arch_lwip_poll. Add the directory containing this file via
// target_include_directories() in your CMakeLists.txt.

// NO_SYS — no RTOS; application drives the poll loop via cyw43_arch_poll()
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// With poll-mode arch, system heap is safe to use for lwIP pools
#define MEM_LIBC_MALLOC             1
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000

// Packet buffer pool — sized for typical WiFi throughput on RP2040
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24

// Networking stack
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_IPV6                   0

// TCP buffer sizes — 8 × MSS gives reasonable throughput on WiFi
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

// mDNS / DNS-SD — advertises the device as <hostname>.local on the LAN
#define LWIP_MDNS_RESPONDER         1
#define MDNS_MAX_SERVICES           1
#define LWIP_NUM_NETIF_CLIENT_DATA  1   // slot for mdns_netif_client_id
#define LWIP_IGMP                   1   // required by MDNS responder for multicast group join
// mDNS + IGMP each register timers; bump the pool to avoid MEMP_SYS_TIMEOUT exhaustion
#define MEMP_NUM_SYS_TIMEOUT        16

// Misc
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_TCP_KEEPALIVE          1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// Random number source for DNS transaction IDs
#define LWIP_RAND()                 ((u32_t)rand())

// Stats — enable in debug builds only
#ifndef NDEBUG
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1
#else
#define LWIP_STATS                  0
#endif

// Debug output — flip individual flags to LWIP_DBG_ON when diagnosing issues
#define LWIP_DEBUG                  0
#define TCP_DEBUG                   LWIP_DBG_OFF
#define ETHARP_DEBUG                LWIP_DBG_OFF
#define PBUF_DEBUG                  LWIP_DBG_OFF
#define IP_DEBUG                    LWIP_DBG_OFF
#define TCP_INPUT_DEBUG             LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG            LWIP_DBG_OFF
#define DHCP_DEBUG                  LWIP_DBG_OFF
#define DNS_DEBUG                   LWIP_DBG_OFF
