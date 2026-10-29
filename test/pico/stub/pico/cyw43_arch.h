#pragma once
// Stub for pico/cyw43_arch.h on Linux test builds: no radio. cyw43_arch_init()
// creates a poll-mode async_context and runs lwIP on it, as the SDK does, so a
// PicoNetwork::Cyw43 runtime also runs on the host. Defined in cyw43_arch_stub.cpp.
#include <pico/async_context.h>
#include <cstdint>

#define CYW43_AUTH_WPA2_AES_PSK 0x00400004

int  cyw43_arch_init();
void cyw43_arch_deinit();
inline void cyw43_arch_enable_sta_mode() {}
inline int  cyw43_arch_wifi_connect_timeout_ms(
    const char*, const char*, uint32_t, uint32_t) { return 0; }

/// The context cyw43_arch_init() created, or null before it / after
/// cyw43_arch_deinit().
async_context_t* cyw43_arch_async_context();

void cyw43_arch_poll();
