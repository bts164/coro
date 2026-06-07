#pragma once
// Stub for pico/cyw43_arch.h — WiFi functions are no-ops on Linux test builds.
// cyw43_arch_poll() is declared here and defined in cyw43_arch_stub.cpp.
// In the executor-only target it is a no-op; in the TCP target it ticks lwIP.
#include <cstdint>

#define CYW43_AUTH_WPA2_AES_PSK 0x00400004

inline int  cyw43_arch_init()            { return 0; }
inline void cyw43_arch_deinit()          {}
inline void cyw43_arch_enable_sta_mode() {}
inline int  cyw43_arch_wifi_connect_timeout_ms(
    const char*, const char*, uint32_t, uint32_t) { return 0; }

void cyw43_arch_poll();
