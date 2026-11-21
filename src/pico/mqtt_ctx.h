#pragma once

// Internal lwIP MQTT context — backing state for coro::MqttClient. Not part of
// the public API; never include from any header under include/coro/.

#include <coro/detail/intrusive_list.h>
#include <coro/detail/waker.h>
#include <coro/detail/rc.h>
#include <coro/io/lwip/lwip_callback_result.h>
#include <coro/pico/mqtt.h>
#include <coro/sync/broadcast.h>
#include <lwip/apps/mqtt.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace coro::detail {

struct MqttCtx;

// One publish() or subscribe() waiting for lwIP's request callback, on that
// coroutine's frame. Listed in MqttCtx::pending until it completes, because lwIP's
// mqtt_close() deletes its pending requests without calling them back (and stops the
// timer that would time them out): MqttCtx::lost() completes them with ERR_CONN.
struct MqttRequest {
    MqttCtx*                  ctx;
    LwipCallbackResult<err_t> result;
    // MqttCtx::pending's links.
    MqttRequest*              prev = nullptr;
    MqttRequest*              next = nullptr;

    explicit MqttRequest(MqttCtx* c);
    ~MqttRequest();
    MqttRequest(const MqttRequest&)            = delete;
    MqttRequest& operator=(const MqttRequest&) = delete;

    // mqtt_request_cb_t, with this request as `arg`.
    static void on_done(void* arg, err_t err);
};

struct MqttCtx {
    mqtt_client_t* client = nullptr;

    // Connect (one-shot — the connection callback fires again later on
    // disconnect, but MqttClient::connect() only awaits the first firing).
    Rc<Waker>                               connect_waker;
    std::optional<mqtt_connection_status_t> connect_status;
    bool                                    connected = false;

    // One broadcast channel per distinct subscribed topic, keyed by exact topic
    // string — set up by MqttClient::subscribe(). Entries are removed only by lost(),
    // which closes them all; until then they are never removed, even
    // once their receiver_count drops to zero: this MqttCtx holds the only sender
    // clone for each entry, so an empty entry is still a fully valid channel a later
    // subscribe() to the same topic can attach to (see kitchen_led.md's "Prerequisite
    // change" section for the full rationale). Exact-match only — no wildcard
    // (`+`/`#`) topic filters in this iteration.
    std::unordered_map<std::string, BroadcastSender<MqttMessage>> channels;

    // Every MqttRequest waiting for lwIP (see MqttRequest). Each links itself in
    // from its own frame, so this never allocates.
    IntrusiveList<MqttRequest*> pending;

    // The connection has dropped, or the client is going: not connected, every pending
    // request fails with ERR_CONN, and every subscription's channel closes (its
    // receivers get Closed). Safe to call more than once.
    void lost();

    // Incoming publish currently being assembled across mqtt_incoming_data_cb_t
    // fragments (lwIP may deliver one publish's payload in several chunks).
    std::string            in_topic;
    std::vector<std::byte> in_payload;

    // lwIP static callbacks
    static void on_connect(mqtt_client_t* client, void* arg, mqtt_connection_status_t status);
    static void on_incoming_publish(void* arg, const char* topic, uint32_t tot_len);
    static void on_incoming_data(void* arg, const uint8_t* data, uint16_t len, uint8_t flags);
};

} // namespace coro::detail
