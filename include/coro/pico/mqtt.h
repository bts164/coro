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
#include <coro/coro_stream.h>
#include <coro/detail/rc.h>
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
 * **Concurrency:** only one `subscribe()` stream may be outstanding at a time — a
 * second call to `subscribe()` displaces the first. This is the same single-consumer
 * restriction `TcpStream::read()` documents for the same reason (one underlying
 * channel, one reader).
 *
 * **Destruction:** destroying an `MqttClient` while `publish()` or a `subscribe()`
 * stream is in flight is UB, same as `TcpStream`.
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
     * @throws std::runtime_error on DNS failure, connection failure, or a non-accepted CONNACK.
     */
    [[nodiscard]] static Coro<MqttClient> connect(
        std::string host, uint16_t port, std::string client_id);

    /**
     * @brief Publishes `payload` to `topic`.
     *
     * Suspends until the broker's PUBACK for QoS 1; for QoS 0 the coroutine resumes
     * as soon as the write is queued (lwIP still invokes the completion callback, but
     * synchronously, so this never actually blocks the caller).
     *
     * @throws std::runtime_error if the client is disconnected, or on publish failure.
     */
    [[nodiscard]] Coro<void> publish(
        std::string topic, std::span<const std::byte> payload,
        uint8_t qos = 0, bool retain = false);

    /**
     * @brief Sends SUBSCRIBE and returns a stream of every message that arrives on
     * `topic` for as long as the returned stream is read. Displaces any previously
     * returned `subscribe()` stream — see the single-consumer note above.
     *
     * @throws std::runtime_error on subscribe failure (e.g. broker refusal).
     */
    [[nodiscard]] CoroStream<MqttMessage> subscribe(std::string topic, uint8_t qos = 0);

private:
    explicit MqttClient(detail::Rc<detail::MqttCtx> impl);

    detail::Rc<detail::MqttCtx> m_impl;
};

} // namespace coro
