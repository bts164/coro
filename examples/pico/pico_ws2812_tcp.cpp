// pico_ws2812_tcp.cpp
//
// Pico W WS2812B LED controller. Connects to WiFi and accepts TCP connections
// on port 2812. Clients send length-prefixed protobuf Command messages and
// receive length-prefixed protobuf Response messages (see proto/ws2812.proto).
// Each frame: 4-byte little-endian length followed by the encoded message body.
//
// Multiple clients are handled concurrently via JoinSet.
//
// Build from examples/pico/build/:
//   Copy wifi_credentials.h.example to wifi_credentials.h and fill in your
//   network details, then:
//     cmake -DPICO_BOARD=pico_w .. && make pico_ws2812_tcp

#include <pico/stdlib.h>
#include <pico/stdio_usb.h>
#include <pico/cyw43_arch.h>
#include <lwip/netif.h>
#include <lwip/apps/mdns.h>

#include "wifi_credentials.h"
#include "lcd.h"
#include "led/led_driver.h"
#include "led/effect_runner.h"
#include "led/effects.h"

#include <coro/runtime/runtime.h>
#include <coro/io/tcp_listener.h>
#include <coro/io/tcp_stream.h>
#include <coro/coro.h>
#include <coro/task/join_handle.h>
#include <coro/task/join_set.h>
#include <coro/sync/select.h>
#include <coro/sync/sleep.h>
#include <coro/sync/watch.h>

#include "ws2812.pb.h"
#include <pb_encode.h>
#include <pb_decode.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

using namespace coro;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static constexpr uint     LED_PIN    = 2;
static constexpr uint     NUM_LEDS   = 8;    // initial active count; changeable via set_leds
static constexpr uint32_t MAX_LEDS   = 300;
static constexpr uint16_t TCP_PORT   = 2812;
static constexpr size_t   MAX_PB_MSG = 4096; // SetPixels for 300 LEDs needs ~1.5 KB

static constexpr uint    LCD_SDA  = 4;
static constexpr uint    LCD_SCL  = 5;
static constexpr uint8_t LCD_ADDR = 0x27;

// ---------------------------------------------------------------------------
// Display state
// ---------------------------------------------------------------------------

struct DisplayState {
    std::string ip      = "Connecting...";
    std::string effect  = "none";
    int         clients = 0;
};

static auto g_display_pair  = watch_channel(DisplayState{});
static auto& g_display_tx   = g_display_pair.first;
static auto& g_display_rx   = g_display_pair.second;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static std::string ms_now() {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%llums",
                  static_cast<unsigned long long>(time_us_64() / 1000));
    return buf;
}

static const char* basename(const char* p) {
    const char* b = p;
    for (; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

#define LOG(__MSG__, ...) \
    std::printf("[%s] %s:%d - " __MSG__ "\n", \
        ms_now().c_str(), basename(__FILE__), __LINE__, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Application state (created in async_main, passed by reference)
// ---------------------------------------------------------------------------

struct AppState {
    std::shared_ptr<led::LedDriver> driver;
    led::EffectRunner&              runner;
    WatchSender<DisplayState>&      display;
};

// ---------------------------------------------------------------------------
// Display loop
// ---------------------------------------------------------------------------

static Coro<void> display_loop(Lcd lcd, WatchReceiver<DisplayState> rx) {
    LOG("Started display_loop");
    auto fmt = [](std::string s) -> std::string {
        if (s.size() > 16) s.resize(16);
        else s.resize(16, ' ');
        return s;
    };

    int page = 0;
    auto s = *rx.borrow_and_update();
    uint32_t timeNext = to_ms_since_boot(get_absolute_time()) + 4000;

    for (;;) {
        std::string line0, line1;
        if (page == 0) {
            line0 = fmt("LED Controller");
            line1 = fmt(s.ip);
        } else {
            char buf[17];
            std::snprintf(buf, sizeof(buf), "Clients: %d", s.clients);
            line0 = fmt("Fx: " + s.effect);
            line1 = fmt(buf);
        }
        lcd.set_cursor(0, 0); lcd.write(line0);
        lcd.set_cursor(1, 0); lcd.write(line1);

        uint32_t timeNow = to_ms_since_boot(get_absolute_time());
        if (timeNow >= timeNext) timeNow = timeNext;

        auto result = co_await select(rx.changed(), sleep_for((timeNext - timeNow) * 1ms));
        if (result.index() == 0) {
            auto s2 = *rx.borrow_and_update();
            if (s.ip != s2.ip) {
                timeNext = to_ms_since_boot(get_absolute_time()) + 4000;
                page = 1;
            }
            s = s2;
        } else {
            page ^= 1;
            timeNext = to_ms_since_boot(get_absolute_time()) + 4000;
        }
    }
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

static Coro<bool> read_command(TcpStream& stream, Command& out) {
    auto hdr = co_await stream.read_exact(std::array<std::byte, 4>{});
    if (hdr.first < 4) co_return false;
    uint32_t len = (uint32_t)hdr.second[0]        | ((uint32_t)hdr.second[1] <<  8) |
                   ((uint32_t)hdr.second[2] << 16) | ((uint32_t)hdr.second[3] << 24);
    if (len > MAX_PB_MSG) co_return false;
    auto body = co_await stream.read_exact(std::string(len, '\0'));
    if (body.first < len) co_return false;
    pb_istream_t is = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(body.second.data()), len);
    out = Command_init_zero;
    co_return pb_decode(&is, Command_fields, &out);
}

static Coro<void> write_response(TcpStream& stream, const Response& resp) {
    uint8_t body[MAX_PB_MSG];
    pb_ostream_t os = pb_ostream_from_buffer(body, sizeof(body));
    if (!pb_encode(&os, Response_fields, &resp)) co_return;
    uint32_t len = (uint32_t)os.bytes_written;
    std::string frame(4 + len, '\0');
    frame[0] = (char)( len        & 0xFF);
    frame[1] = (char)((len >>  8) & 0xFF);
    frame[2] = (char)((len >> 16) & 0xFF);
    frame[3] = (char)((len >> 24) & 0xFF);
    std::memcpy(frame.data() + 4, body, len);
    co_await stream.write(std::move(frame));
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------

static Response ok_response() {
    Response r = Response_init_zero;
    r.which_result = Response_ok_tag;
    return r;
}

static Response error_response(const char* msg) {
    Response r = Response_init_zero;
    r.which_result = Response_error_tag;
    std::strncpy(r.result.error, msg, sizeof(r.result.error) - 1);
    return r;
}

// ---------------------------------------------------------------------------
// Command handling
// ---------------------------------------------------------------------------

static Coro<Response> handle_command(const Command& cmd, AppState& app) {
    switch (cmd.which_cmd) {

    case Command_fill_tag: {
        co_await app.runner.stop();
        led::Color c{
            static_cast<uint8_t>(std::min<uint32_t>(cmd.cmd.fill.r, 255u)),
            static_cast<uint8_t>(std::min<uint32_t>(cmd.cmd.fill.g, 255u)),
            static_cast<uint8_t>(std::min<uint32_t>(cmd.cmd.fill.b, 255u)),
        };
        led::PixelBuffer frame(app.driver->led_count(), c.to_grb_word());
        co_await app.driver->show(frame);
        co_return ok_response();
    }

    case Command_set_tag: {
        const SetPixel& s = cmd.cmd.set;
        if (s.index >= app.driver->led_count())
            co_return error_response("index out of range");
        co_await app.runner.stop();
        led::PixelBuffer frame = app.driver->buffer();
        frame.resize(app.driver->led_count(), 0u);
        frame[s.index] = led::Color{
            static_cast<uint8_t>(std::min<uint32_t>(s.color.r, 255u)),
            static_cast<uint8_t>(std::min<uint32_t>(s.color.g, 255u)),
            static_cast<uint8_t>(std::min<uint32_t>(s.color.b, 255u)),
        }.to_grb_word();
        co_await app.driver->show(frame);
        co_return ok_response();
    }

    case Command_set_pixels_tag: {
        co_await app.runner.stop();
        led::PixelBuffer frame = app.driver->buffer();
        frame.resize(app.driver->led_count(), 0u);
        const SetPixels& sp = cmd.cmd.set_pixels;
        for (pb_size_t i = 0; i < sp.pixels_count; ++i) {
            const SetPixel& s = sp.pixels[i];
            if (s.index < app.driver->led_count()) {
                frame[s.index] = led::Color{
                    static_cast<uint8_t>(std::min<uint32_t>(s.color.r, 255u)),
                    static_cast<uint8_t>(std::min<uint32_t>(s.color.g, 255u)),
                    static_cast<uint8_t>(std::min<uint32_t>(s.color.b, 255u)),
                }.to_grb_word();
            }
        }
        co_await app.driver->show(frame);
        co_return ok_response();
    }

    case Command_clear_tag: {
        co_await app.runner.stop();
        led::PixelBuffer frame(app.driver->led_count(), 0u);
        co_await app.driver->show(frame);
        co_return ok_response();
    }

    case Command_show_tag:
        co_await app.runner.stop();
        co_await app.driver->show(app.driver->buffer());
        co_return ok_response();

    case Command_stop_tag:
        co_await app.runner.stop();
        app.display.borrow_mut()->effect = "none";
        co_return ok_response();

    case Command_brightness_tag:
        app.driver->set_brightness(
            static_cast<uint8_t>(std::min<uint32_t>(cmd.cmd.brightness, 255u)));
        co_return ok_response();

    case Command_get_leds_tag: {
        Response r = Response_init_zero;
        r.which_result = Response_led_count_tag;
        r.result.led_count = static_cast<uint32_t>(app.driver->led_count());
        co_return r;
    }

    case Command_set_leds_tag: {
        uint32_t n = cmd.cmd.set_leds;
        if (n < 1 || n > MAX_LEDS) co_return error_response("leds out of range");
        app.driver->set_led_count(static_cast<std::size_t>(n));
        co_return ok_response();
    }

    case Command_fade_tag:
        co_await app.runner.set_effect(led::fade_effect(app.driver, cmd.cmd.fade));
        app.display.borrow_mut()->effect = "fade";
        co_return ok_response();

    case Command_marquee_tag:
        co_await app.runner.set_effect(led::marquee_effect(app.driver, cmd.cmd.marquee));
        app.display.borrow_mut()->effect = "marquee";
        co_return ok_response();

    case Command_comet_tag:
        co_await app.runner.set_effect(led::comet_effect(app.driver, cmd.cmd.comet));
        app.display.borrow_mut()->effect = "comet";
        co_return ok_response();

    case Command_stars_tag:
        co_await app.runner.set_effect(led::stars_effect(app.driver, cmd.cmd.stars));
        app.display.borrow_mut()->effect = "stars";
        co_return ok_response();

    case Command_bounce_tag:
        co_await app.runner.set_effect(led::bounce_effect(app.driver, cmd.cmd.bounce));
        app.display.borrow_mut()->effect = "bounce";
        co_return ok_response();

    case Command_particles_tag:
        co_await app.runner.set_effect(led::particles_effect(app.driver, cmd.cmd.particles));
        app.display.borrow_mut()->effect = "particles";
        co_return ok_response();

    case Command_waves_tag:
        co_await app.runner.set_effect(led::waves_effect(app.driver, cmd.cmd.waves));
        app.display.borrow_mut()->effect = "waves";
        co_return ok_response();

    case Command_spring_tag:
        co_await app.runner.set_effect(led::spring_effect(app.driver, cmd.cmd.spring));
        app.display.borrow_mut()->effect = "spring";
        co_return ok_response();

    case Command_plasma_tag:
        co_await app.runner.set_effect(led::plasma_effect(app.driver, cmd.cmd.plasma));
        app.display.borrow_mut()->effect = "plasma";
        co_return ok_response();

    default:
        co_return error_response("unknown command");
    }
}

// ---------------------------------------------------------------------------
// Per-connection coroutine
// ---------------------------------------------------------------------------

static Coro<void> handle_connection(TcpStream stream, int id, AppState& app) {
    app.display.borrow_mut()->clients++;
    LOG("client %d connected", id);

    for (;;) {
        Command cmd;
        if (!co_await read_command(stream, cmd)) break;
        Response resp = co_await handle_command(cmd, app);
        co_await write_response(stream, resp);
    }

    LOG("client %d disconnected", id);
    app.display.borrow_mut()->clients--;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static Coro<int> async_main(Lcd lcd) {
    LOG("Started async_main");

    std::shared_ptr<led::LedDriver> driver = co_await led::LedDriver::create(LED_PIN, NUM_LEDS);
    led::EffectRunner runner(driver);
    AppState app{driver, runner, g_display_tx};

    spawn(display_loop(std::move(lcd), std::move(g_display_rx))).detach();
    LOG("Spawned display_loop");

    {
        auto s = g_display_tx.borrow_mut();
        s->ip = ip4addr_ntoa(netif_ip4_addr(netif_default));
        std::printf("WiFi connected. IP: %s\n", s->ip.c_str());
    }

    LOG("binding on port %d...", TCP_PORT);
    TcpListener listener = co_await TcpListener::bind("0.0.0.0", TCP_PORT);
    LOG("listening on port %d (LED_PIN=%u NUM_LEDS=%zu)", TCP_PORT, LED_PIN, driver->led_count());

    JoinSet<void> sessions;
    for (int id = 0;; ++id) {
        TcpStream stream = co_await listener.accept();
        sessions.spawn(handle_connection(std::move(stream), id, app));
    }
    co_return 0;
}

int main() {
    stdio_init_all();
    for (int i = 0; i < 50 && !stdio_usb_connected(); ++i)
        sleep_ms(100);

    Lcd lcd(i2c0, LCD_ADDR, LCD_SDA, LCD_SCL);
    lcd.init();

    auto fmt = [](std::string s) -> std::string {
        if (s.size() > 16) s.resize(16);
        else s.resize(16, ' ');
        return s;
    };

    if (cyw43_arch_init()) {
        std::printf("cyw43_arch_init failed\n");
        lcd.set_cursor(0, 0); lcd.write(fmt("cyw43 init fail"));
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    std::printf("connecting to WiFi '%s'...\n", WIFI_SSID);
    lcd.set_cursor(0, 0); lcd.write(fmt("Connecting WiFi"));
    lcd.set_cursor(1, 0); lcd.write(fmt(WIFI_SSID));
    while (cyw43_arch_wifi_connect_timeout_ms(
               WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 15000)) {
        std::printf("WiFi connect failed, retrying...\n");
        lcd.set_cursor(0, 0); lcd.write(fmt("WiFi retry...   "));
        sleep_ms(2000);
    }
    std::printf("connected to WiFi '%s'...\n", WIFI_SSID);

    mdns_resp_init();
    mdns_resp_add_netif(netif_default, DEVICE_NAME);
    mdns_resp_add_service(netif_default, DEVICE_NAME, "_tcp",
                          DNSSD_PROTO_TCP, TCP_PORT, nullptr, nullptr);
    std::printf("mDNS: reachable as %s.local\n", DEVICE_NAME);

    coro::Runtime rt;
    int r = rt.block_on(async_main(std::move(lcd)));
    cyw43_arch_deinit();
    return r;
}
