#pragma once

// Coroutine wrapper over lwIP's apps/mqtt client (lwip/apps/mqtt.h). Part of the
// optional coro_pico_mqtt module — see doc/design/pico_port.md, "Optional MQTT
// module: coro_pico_mqtt".
//
// All lwIP MQTT callbacks fire synchronously on the executor thread during
// cyw43_arch_poll() (Pico) / sys_check_timeouts() (host test builds), same as
// TcpStream — no mutex required. See LwipCallbackResult's caveat about
// CYW43_ARCH_THREADSAFE_BACKGROUND in lwip_callback_result.h.
//
// To integrate in a project:
//   target_link_libraries(my_firmware PRIVATE coro_pico coro_pico_mqtt)
//   #define LWIP_MQTT 1   // in lwipopts.h

#include <coro/coro.h>
#include <coro/detail/rc.h>
#include <coro/sync/broadcast.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace coro {

namespace detail { struct MqttCtx; }

/// @brief One message delivered to a subscribed topic.
struct MqttMessage {
    std::string            topic;
    std::vector<std::byte> payload;
};

/**
 * @brief Async MQTT client. Move-only; obtain via `co_await MqttClient::connect()`.
 *
 * Wraps lwIP's `apps/mqtt` client. Only QoS 0/1 are supported — lwIP itself does not
 * implement QoS 2 or persistent sessions (`clean_session` is always set on CONNECT).
 *
 * **Concurrency:** `subscribe()` may be called for any number of distinct topics
 * concurrently — each call returns its own `BroadcastReceiver<MqttMessage>`, filtered
 * to that exact topic string, independent of every other subscription. Calling
 * `subscribe()` again with the *same* topic string returns another independent
 * receiver on the same underlying channel (both see every message on that topic from
 * the point they subscribed onward) — it does not displace anything.
 *
 * **Disconnection:** once the connection drops (the broker closes it, the TCP
 * connection fails, or the keep-alive finds the broker unresponsive), `connected()`
 * is false, every `publish()` and `subscribe()` still waiting throws, later ones throw
 * at once, and every receiver from `subscribe()` gets `BroadcastRecvError::Closed` once
 * it has read what was already delivered. There is no reconnect: connect a new client.
 * A task reading a subscription is therefore told about the disconnect too.
 *
 * **Destruction:** destroying an `MqttClient` disconnects it, with the same effects as
 * above. lwIP closes the TCP connection without sending DISCONNECT, so the broker
 * publishes the Last Will, if there is one. A `publish()` or `subscribe()` must not be cancelled (its
 * coroutine dropped) while waiting, same as `TcpStream`: lwIP still holds its
 * callback. Outstanding `BroadcastReceiver`s from `subscribe()` may safely outlive or
 * be dropped independently of the `MqttClient` and of each other.
 */
class MqttClient {
public:
    MqttClient(MqttClient&&) noexcept;
    MqttClient& operator=(MqttClient&&) noexcept;
    MqttClient(const MqttClient&)            = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    ~MqttClient();

    /**
     * @brief Resolves host, opens the TCP connection, and performs the MQTT CONNECT handshake.
     *
     * @param will_topic  If non-empty, registered as the connection's Last Will and
     *                    Testament: the broker publishes `will_payload` to `will_topic`
     *                    if this client disconnects uncleanly. Empty (the default) disables
     *                    the LWT entirely — `will_payload`/`will_qos`/`will_retain` are then
     *                    ignored.
     * @param username    If non-empty, sent as the CONNECT packet's User Name field. Empty
     *                    (the default) omits it — the User Name Flag is left clear.
     * @param password    If non-empty, sent as the CONNECT packet's Password field. Empty
     *                    (the default) omits it — the Password Flag is left clear. Ignored
     *                    if `username` is empty, since lwIP's MQTT client only sends a
     *                    password alongside a user name.
     * @param keep_alive_s The CONNECT packet's Keep Alive, in seconds; 0 (the default)
     *                    turns keep-alive off. Non-zero, lwIP sends PINGREQ whenever it has
     *                    sent nothing for that long, and drops the connection if it hears
     *                    nothing from the broker for 1.5 times that; the broker likewise
     *                    drops a client it hasn't heard from for 1.5 times that, and
     *                    publishes its Last Will. Without it, a client that loses power
     *                    or its network is never noticed by the broker, and its Last Will
     *                    never fires.
     * @throws std::runtime_error on DNS failure, connection failure, or a non-accepted CONNACK.
     */
    [[nodiscard]] static Coro<MqttClient> connect(
        std::string host, uint16_t port, std::string client_id,
        std::string will_topic = {}, std::string will_payload = {},
        uint8_t will_qos = 0, bool will_retain = false,
        std::string username = {}, std::string password = {},
        uint16_t keep_alive_s = 0);

    /**
     * @brief Publishes `payload` to `topic`.
     *
     * Suspends until the broker's PUBACK for QoS 1; for QoS 0 the coroutine resumes
     * as soon as the write is queued (lwIP still invokes the completion callback, but
     * synchronously, so this never actually blocks the caller).
     *
     * @throws std::runtime_error if the client is disconnected or disconnects before the
     *         publish completes, or on publish failure.
     */
    [[nodiscard]] Coro<void> publish(
        std::string topic, std::span<const std::byte> payload,
        uint8_t qos = 0, bool retain = false);

    /**
     * @brief Sends SUBSCRIBE and returns a receiver of every message that arrives on
     * `topic` from this point onward.
     *
     * Internally, one broadcast channel is kept per distinct topic string for the
     * lifetime of the `MqttClient`; `capacity` only takes effect the first time a given
     * topic is subscribed (later calls for the same topic ignore it and just attach
     * another receiver to the channel that already exists). See the class-level
     * concurrency note — multiple topics, and multiple receivers on the same topic, may
     * all be live at once.
     *
     * @throws std::runtime_error if the client is disconnected or disconnects before the
     *         SUBACK, or on subscribe failure (e.g. broker refusal).
     */
    [[nodiscard]] Coro<BroadcastReceiver<MqttMessage>> subscribe(
        std::string topic, uint8_t qos = 0, size_t capacity = 8);

    /// @brief False once the connection has dropped (see **Disconnection** above), or
    /// for a moved-from client. Never true again: connect a new client.
    [[nodiscard]] bool connected() const;

private:
    explicit MqttClient(detail::Rc<detail::MqttCtx> impl);

    detail::Rc<detail::MqttCtx> m_impl;
};

} // namespace coro
