// lwIP-backed MqttClient implementation for NO_SYS mode.
//
// Compile as part of the application via the coro_pico_mqtt cmake target:
//   target_link_libraries(my_app PRIVATE coro_pico coro_pico_mqtt)
//
// All lwIP MQTT callbacks fire synchronously on the executor thread during
// cyw43_arch_poll() (Pico) / sys_check_timeouts() (host test builds), same
// pattern as tcp_stream_lwip.cpp.

#include <coro/pico/mqtt.h>
#include <coro/io/lwip/lwip_callback_result.h>
#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include "mqtt_ctx.h"
#include <lwip/dns.h>
#include <lwip/err.h>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// MqttCtx callbacks  (coro::detail namespace)
// ---------------------------------------------------------------------------

namespace coro::detail {

void MqttCtx::on_connect(mqtt_client_t*, void* arg, mqtt_connection_status_t status) {
    auto* ctx = static_cast<MqttCtx*>(arg);

    if (!ctx->connect_status.has_value()) {
        // First firing — completes MqttClient::connect()'s co_await.
        ctx->connect_status = status;
        ctx->connected       = (status == MQTT_CONNECT_ACCEPTED);
        if (ctx->connect_waker) {
            auto w = std::move(ctx->connect_waker);
            w->wake();
        }
        return;
    }

    // Later firing — server-initiated disconnect or keep-alive timeout.
    // RACE CONDITION NOTE: safe — this callback and every coro caller run on
    // the same executor thread, never concurrently.
    ctx->connected = false;
}

void MqttCtx::on_incoming_publish(void* arg, const char* topic, uint32_t /*tot_len*/) {
    auto* ctx = static_cast<MqttCtx*>(arg);
    ctx->in_topic = topic;
    ctx->in_payload.clear();
}

void MqttCtx::on_incoming_data(void* arg, const uint8_t* data, uint16_t len, uint8_t flags) {
    auto* ctx = static_cast<MqttCtx*>(arg);

    const auto old_size = ctx->in_payload.size();
    ctx->in_payload.resize(old_size + len);
    std::memcpy(ctx->in_payload.data() + old_size, data, len);

    if (!(flags & MQTT_DATA_FLAG_LAST))
        return;

    // Message complete. Deliver to the channel registered for this exact topic, if
    // any — see the `channels` map note in mqtt_ctx.h. No entry means nobody is
    // subscribed to this topic; the message is simply dropped. Best-effort: if the
    // ring buffer is full, send() silently overwrites the oldest unread value rather
    // than blocking this callback, which cannot suspend.
    auto it = ctx->channels.find(ctx->in_topic);
    if (it != ctx->channels.end()) {
        it->second.send(MqttMessage{ctx->in_topic, std::move(ctx->in_payload)});
    }
    ctx->in_payload.clear();
}

} // namespace coro::detail

// ---------------------------------------------------------------------------
// MqttClient methods  (coro namespace)
// ---------------------------------------------------------------------------

namespace coro {

MqttClient::MqttClient(detail::Rc<detail::MqttCtx> impl) : m_impl(std::move(impl)) {}
MqttClient::MqttClient(MqttClient&&) noexcept            = default;
MqttClient& MqttClient::operator=(MqttClient&&) noexcept = default;

MqttClient::~MqttClient() {
    if (!m_impl || !m_impl->client) return;
    mqtt_disconnect(m_impl->client);
    mqtt_client_free(m_impl->client);
    m_impl->client = nullptr;
}

Coro<MqttClient> MqttClient::connect(
    std::string host, uint16_t port, std::string client_id,
    std::string will_topic, std::string will_payload, uint8_t will_qos, bool will_retain,
    std::string username, std::string password) {
    // ---- Step 1: DNS resolution (same pattern as TcpStream::connect) ----
    LwipCallbackResult<ip_addr_t, bool> dns_cb;
    ip_addr_t addr{};

    err_t dns_err = dns_gethostbyname(
        host.c_str(), &addr,
        +[](const char*, const ip_addr_t* resolved, void* arg) {
            auto* cb = static_cast<LwipCallbackResult<ip_addr_t, bool>*>(arg);
            if (resolved) cb->complete(*resolved, true);
            else          cb->complete(ip_addr_t{}, false);
        },
        &dns_cb);

    if (dns_err == ERR_INPROGRESS) {
        auto [resolved, ok] = co_await lwip_wait(dns_cb);
        if (!ok) throw std::runtime_error("MqttClient::connect: DNS lookup failed");
        addr = resolved;
    } else if (dns_err != ERR_OK) {
        throw std::runtime_error("MqttClient::connect: dns_gethostbyname failed");
    }

    // ---- Step 2: MQTT CONNECT ----
    auto ctx = detail::make_rc<detail::MqttCtx>();
    ctx->client = mqtt_client_new();
    if (!ctx->client) throw std::runtime_error("MqttClient::connect: mqtt_client_new failed");

    mqtt_connect_client_info_t info{};
    info.client_id = client_id.c_str();
    if (!will_topic.empty()) {
        info.will_topic = will_topic.c_str();
        info.will_msg   = will_payload.c_str();
        info.will_qos   = will_qos;
        info.will_retain = will_retain ? 1 : 0;
    }
    if (!username.empty()) {
        info.client_user = username.c_str();
        if (!password.empty())
            info.client_pass = password.c_str();
    }

    err_t conn_err = mqtt_client_connect(
        ctx->client, &addr, port, detail::MqttCtx::on_connect, ctx.get(), &info);
    if (conn_err != ERR_OK) {
        mqtt_client_free(ctx->client);
        ctx->client = nullptr;
        throw std::runtime_error("MqttClient::connect: mqtt_client_connect failed");
    }

    struct ConnectWait {
        using OutputType = void;
        detail::Rc<detail::MqttCtx> ctx;

        PollResult<void> poll(detail::Context& cx) {
            if (ctx->connect_status.has_value()) return PollReady;
            ctx->connect_waker = cx.getWaker();
            return PollPending;
        }
    };

    if (!ctx->connect_status.has_value())
        co_await ConnectWait{ctx};

    if (*ctx->connect_status != MQTT_CONNECT_ACCEPTED) {
        auto status = *ctx->connect_status;
        mqtt_client_free(ctx->client);
        ctx->client = nullptr;
        throw std::runtime_error(
            "MqttClient::connect: broker refused connection, status=" +
            std::to_string(static_cast<int>(status)));
    }

    // Register the single client-wide incoming-publish callback pair up front;
    // subscribe() only ever changes which topic/channel routes data, never
    // re-registers with lwIP.
    mqtt_set_inpub_callback(
        ctx->client, detail::MqttCtx::on_incoming_publish, detail::MqttCtx::on_incoming_data, ctx.get());

    co_return MqttClient(std::move(ctx));
}

Coro<void> MqttClient::publish(
    std::string topic, std::span<const std::byte> payload, uint8_t qos, bool retain) {
    // Capture ctx before any suspension so we don't hold a dangling `this` if
    // the MqttClient object is moved while this coroutine is parked.
    auto ctx_ptr = m_impl;

    if (!ctx_ptr->connected)
        throw std::runtime_error("MqttClient::publish: not connected");

    LwipCallbackResult<err_t> result;
    err_t err = mqtt_publish(
        ctx_ptr->client, topic.c_str(), payload.data(), static_cast<uint16_t>(payload.size()),
        qos, retain ? 1 : 0,
        +[](void* arg, err_t e) {
            static_cast<LwipCallbackResult<err_t>*>(arg)->complete(e);
        },
        &result);

    if (err != ERR_OK)
        throw std::runtime_error("MqttClient::publish: mqtt_publish failed");

    auto [ack] = co_await lwip_wait(result);
    if (ack != ERR_OK)
        throw std::runtime_error("MqttClient::publish: broker did not acknowledge");
}

Coro<BroadcastReceiver<MqttMessage>> MqttClient::subscribe(
    std::string topic, uint8_t qos, size_t capacity) {
    // Capture ctx before any suspension so we don't hold a dangling `this` if
    // the MqttClient object is moved while this coroutine is parked.
    auto ctx_ptr = m_impl;

    if (!ctx_ptr->connected)
        throw std::runtime_error("MqttClient::subscribe: not connected");

    LwipCallbackResult<err_t> result;
    err_t err = mqtt_subscribe(
        ctx_ptr->client, topic.c_str(), qos,
        +[](void* arg, err_t e) {
            static_cast<LwipCallbackResult<err_t>*>(arg)->complete(e);
        },
        &result);

    if (err != ERR_OK)
        throw std::runtime_error("MqttClient::subscribe: mqtt_subscribe failed");

    // Find-or-create the channel for this exact topic and attach a receiver to it
    // before awaiting the SUBACK below: on_incoming_data() can fire as soon as the
    // broker acks the subscription, so the channel entry must already exist or an
    // immediately-following PUBLISH is silently dropped. See the `channels` map note
    // in mqtt_ctx.h.
    std::optional<BroadcastReceiver<MqttMessage>> rx;
    auto it = ctx_ptr->channels.find(topic);
    if (it != ctx_ptr->channels.end()) {
        rx = it->second.subscribe();
    } else {
        auto chan = broadcast_channel<MqttMessage>(capacity);
        rx = std::move(chan.second);
        ctx_ptr->channels.emplace(topic, std::move(chan.first));
    }

    auto [ack] = co_await lwip_wait(result);
    if (ack != ERR_OK)
        throw std::runtime_error("MqttClient::subscribe: broker refused subscription");

    co_return std::move(*rx);
}

} // namespace coro
