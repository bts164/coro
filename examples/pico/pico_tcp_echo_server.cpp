// pico_tcp_echo_server.cpp
//
// Pico W TCP echo server. Binds to 0.0.0.0:8080 and echoes back every message
// received from any connected client. Handles multiple concurrent connections
// via JoinSet.
//
// Build with the Pico SDK. Pass WiFi credentials at configure time:
//
//   cmake -DWIFI_SSID="myssid" -DWIFI_PASSWORD="mypassword" ...
//
// Connect from a desktop machine:
//   echo "hello" | nc <pico-ip> 8080

#include <pico/stdlib.h>
#include <pico/stdio_usb.h>
#include <pico/cyw43_arch.h>
#include <lwip/netif.h>
#include <coro/runtime/runtime.h>
#include <coro/io/tcp_listener.h>
#include <coro/io/tcp_stream.h>
#include <coro/coro.h>
#include <coro/task/join_set.h>
#include <coro/sync/timeout.h>
#include <cstdio>
#include <string>

using namespace coro;

#ifndef WIFI_SSID
#define WIFI_SSID     "your_wifi_ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your_wifi_password"
#endif
static constexpr const char* BIND_HOST = "0.0.0.0";
static constexpr uint16_t    BIND_PORT = 8080;

// ---------------------------------------------------------------------------
// Logging — milliseconds since boot (no wall clock on bare metal)
// ---------------------------------------------------------------------------

static const char* path_basename(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

static std::string timestamp() {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%llums",
                  static_cast<unsigned long long>(time_us_64() / 1000));
    return buf;
}

#define LOG(__ID__, __MSG__, ...) \
    std::printf("[%s] %s:%d %d - " __MSG__ "\n", \
        timestamp().c_str(), path_basename(__FILE__), __LINE__, __ID__, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// handle_connection
// ---------------------------------------------------------------------------

static Coro<void> handle_connection(TcpStream stream, int id) {
    LOG(id, "handle_connection started");
    struct Defer {
        Defer(int id) : id_(id) {}
        ~Defer() { LOG(id_, "Connection closed"); }
        int id_;
    } defer(id);

    for (;;) {
        LOG(id, "waiting for read...");
        auto [n, buf] = co_await stream.read(std::string(4096, '\0'));
        LOG(id, "read returned n=%zu", n);
        if (n == 0) { LOG(id, "EOF"); co_return; }
        buf.resize(n);
        LOG(id, "Received \"%s\"", buf.c_str());
        co_await stream.write(std::move(buf));
        LOG(id, "Echoed");
    }
}

// ---------------------------------------------------------------------------
// run_server
// ---------------------------------------------------------------------------

static Coro<void> run_server() {
    using namespace std::chrono_literals;

    LOG(-1, "Binding to %s:%d...", BIND_HOST, BIND_PORT);
    TcpListener listener = co_await TcpListener::bind(BIND_HOST, BIND_PORT);
    LOG(-1, "Listening on %s:%d", BIND_HOST, BIND_PORT);

    JoinSet<void> sessions;
    for (int i = 0;;) {
        auto result = co_await coro::timeout(5s, listener.accept());
        if (result.index() != 0) {
            LOG(-1, "Still waiting for connection...");
            continue;
        }
        TcpStream stream = std::move(std::get<0>(result).value);
        LOG(-1, "Accepted connection %d", i);
        sessions.spawn(handle_connection(std::move(stream), i));
        ++i;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    stdio_init_all();
    for (int i = 0; i < 50 && !stdio_usb_connected(); ++i)
        sleep_ms(100);

    if (cyw43_arch_init()) {
        std::printf("cyw43_arch_init failed\n");
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    std::printf("Connecting to WiFi '%s'...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 15000)) {
        std::printf("WiFi connect failed\n");
        cyw43_arch_deinit();
        return 1;
    }
    std::printf("WiFi connected. IP: %s  Starting echo server on port %d...\n",
                ip4addr_ntoa(netif_ip4_addr(netif_default)), BIND_PORT);

    coro::Runtime rt;
    rt.block_on(run_server());

    cyw43_arch_deinit();
    return 0;
}
