// pico_tcp_echo_client.cpp
//
// Pico W TCP echo client. Connects to a running echo server, sends 5 messages,
// and prints the replies.
//
// Build with the Pico SDK. Pass WiFi credentials and server address at
// configure time:
//
//   cmake -DWIFI_SSID="myssid" -DWIFI_PASSWORD="mypassword" \
//         -DECHO_HOST="192.168.1.100" ...

#include <pico/stdlib.h>
#include <pico/stdio_usb.h>
#include <pico/cyw43_arch.h>
#include <coro/runtime/runtime.h>
#include <coro/io/tcp_stream.h>
#include <coro/coro.h>
#include <coro/task/join_set.h>
#include <cstdio>
#include <string>

using namespace coro;

#ifndef WIFI_SSID
#define WIFI_SSID     "your_wifi_ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your_wifi_password"
#endif
#ifndef ECHO_HOST
#define ECHO_HOST     "192.168.1.100"
#endif
static constexpr uint16_t ECHO_PORT = 8080;

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
// run_client
// ---------------------------------------------------------------------------

static Coro<void> run_client(int id, std::string message) {
    struct Defer {
        Defer(int id) : id_(id) {}
        ~Defer() { LOG(id_, "Connection closed"); }
        int id_;
    } defer(id);

    TcpStream stream = co_await TcpStream::connect(ECHO_HOST, ECHO_PORT);
    LOG(id, "Connected to " ECHO_HOST ":%d", ECHO_PORT);

    for (size_t i = 0; i < 5; ++i) {
        char msg_buf[128];
        std::snprintf(msg_buf, sizeof(msg_buf),
                      "%s from %d iteration %zu", message.c_str(), id, i);
        std::string msg(msg_buf);
        LOG(id, "Sending: %s", msg.c_str());

        co_await stream.write(std::move(msg));

        auto [n, reply] = co_await stream.read(std::string(4096, '\0'));
        if (n == 0) { LOG(id, "Server closed connection"); co_return; }
        reply.resize(n);
        LOG(id, "Echo: %s", reply.c_str());
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
    std::printf("WiFi connected\n");

    coro::Runtime rt;
    rt.block_on(run_client(0, "hello pico"));

    cyw43_arch_deinit();
    return 0;
}
