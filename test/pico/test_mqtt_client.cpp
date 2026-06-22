// Real lwIP MQTT tests — host NO_SYS loopback, same pattern as
// test_tcp_stream_real.cpp. lwIP ships no broker implementation (only the
// client wrapped by MqttClient), so each test below drives a small hand-rolled
// fake broker over coro::TcpListener that speaks just enough of the MQTT wire
// format to exercise one path: CONNECT/CONNACK, SUBSCRIBE/SUBACK plus a
// scripted PUBLISH back to the client, and a QoS 0 PUBLISH from the client
// (which completes locally once lwIP's TCP send completes — no broker
// response needed; see mqtt_tcp_sent_cb in lwIP's mqtt.c).

#include <coro/pico/mqtt.h>
#include <coro/io/tcp_listener.h>
#include <coro/io/tcp_stream.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <gtest/gtest.h>

#include <lwip/init.h>
#include <lwip/timeouts.h>
#include <lwip/netif.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Minimal MQTT control-packet builders. Only single-byte remaining-length
// encoding is needed — every packet built or parsed here is well under 128
// bytes, which is the only case exercised by these tests.
// ---------------------------------------------------------------------------

std::string build_connack() {
    return std::string{"\x20\x02\x00\x00", 4}; // CONNACK, len 2, session_present=0, rc=accepted
}

std::string build_suback(uint16_t pkt_id, uint8_t granted_qos) {
    std::string p;
    p += '\x90';                                     // SUBACK
    p += '\x03';                                      // remaining length
    p += static_cast<char>((pkt_id >> 8) & 0xFF);
    p += static_cast<char>(pkt_id & 0xFF);
    p += static_cast<char>(granted_qos);
    return p;
}

// QoS 0 PUBLISH from broker to client — no packet identifier.
std::string build_publish_qos0(const std::string& topic, const std::string& payload) {
    std::string remaining;
    remaining += static_cast<char>((topic.size() >> 8) & 0xFF);
    remaining += static_cast<char>(topic.size() & 0xFF);
    remaining += topic;
    remaining += payload;

    std::string p;
    p += '\x30'; // PUBLISH, qos=0, dup=0, retain=0
    p += static_cast<char>(remaining.size()); // < 128 for every test fixture below
    p += remaining;
    return p;
}

// Reads the 2-byte big-endian packet identifier out of a SUBSCRIBE packet's
// variable header (immediately after the 1-byte remaining-length field).
uint16_t extract_subscribe_pkt_id(const std::string& packet) {
    return static_cast<uint16_t>(static_cast<uint8_t>(packet[2]) << 8) |
           static_cast<uint8_t>(packet[3]);
}

// Reads the 2-byte big-endian packet identifier out of a QoS>0 PUBLISH
// packet's variable header: topic-length-prefixed topic, then packet id
// (only present when QoS > 0 — absent for the QoS 0 packets built above).
uint16_t extract_publish_pkt_id_qos1(const std::string& packet) {
    uint16_t topic_len = static_cast<uint16_t>(static_cast<uint8_t>(packet[2]) << 8) |
                          static_cast<uint8_t>(packet[3]);
    std::size_t idx = 4 + topic_len;
    return static_cast<uint16_t>(static_cast<uint8_t>(packet[idx]) << 8) |
           static_cast<uint8_t>(packet[idx + 1]);
}

std::string build_puback(uint16_t pkt_id) {
    std::string p;
    p += '\x40'; // PUBACK
    p += '\x02'; // remaining length
    p += static_cast<char>((pkt_id >> 8) & 0xFF);
    p += static_cast<char>(pkt_id & 0xFF);
    return p;
}

// Decodes just enough of a CONNECT packet's variable header + payload to verify
// LWT registration: the connect-flags byte (bit 2 = will flag) and, if set, the
// will topic/message. Assumes a 1-byte remaining-length field, true for every
// CONNECT built by MqttClient::connect() in these tests.
struct ParsedConnect {
    uint8_t                    flags = 0;
    std::optional<std::string> will_topic;
    std::optional<std::string> will_message;
    std::optional<std::string> username;
    std::optional<std::string> password;
};

ParsedConnect parse_connect(const std::string& packet) {
    ParsedConnect out;
    std::size_t idx = 2;       // fixed header
    idx += 2 + 4;               // protocol name length + "MQTT"
    idx += 1;                   // protocol level
    out.flags = static_cast<uint8_t>(packet[idx]);
    idx += 1;
    idx += 2;                   // keep alive

    auto read_u16 = [&packet](std::size_t i) {
        return static_cast<uint16_t>(static_cast<uint8_t>(packet[i]) << 8) |
               static_cast<uint8_t>(packet[i + 1]);
    };

    uint16_t cid_len = read_u16(idx);
    idx += 2 + cid_len;          // skip client id

    if (out.flags & 0x04) {      // will flag
        uint16_t wt_len = read_u16(idx);
        idx += 2;
        out.will_topic = packet.substr(idx, wt_len);
        idx += wt_len;

        uint16_t wm_len = read_u16(idx);
        idx += 2;
        out.will_message = packet.substr(idx, wm_len);
        idx += wm_len;
    }

    if (out.flags & 0x80) {       // user name flag
        uint16_t un_len = read_u16(idx);
        idx += 2;
        out.username = packet.substr(idx, un_len);
        idx += un_len;
    }

    if (out.flags & 0x40) {       // password flag
        uint16_t pw_len = read_u16(idx);
        idx += 2;
        out.password = packet.substr(idx, pw_len);
        idx += pw_len;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture — initialises lwIP once per process.
// ---------------------------------------------------------------------------

class MqttLoopback : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        lwip_init();
    }

    static void poll_until(coro::Runtime& rt, bool& flag, int max_iters = 100'000) {
        for (int i = 0; i < max_iters && !flag; ++i) {
            rt.poll();
            sys_check_timeouts();
            netif_poll_all();
        }
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(MqttLoopback, ConnectAccepted) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19890);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();
            auto [n, buf] = co_await stream.read(std::string(256, '\0')); // CONNECT
            (void)n;
            co_await stream.write(build_connack());
        }(std::move(listener))).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19890, "test-client");
            (void)client;
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "MqttClient::connect threw unexpectedly";
}

TEST_F(MqttLoopback, PublishQos0) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19891);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();
            auto [n, buf] = co_await stream.read(std::string(256, '\0')); // CONNECT
            (void)n;
            co_await stream.write(build_connack());
            // QoS 0 publish needs no broker response — just keep draining so
            // the connection doesn't back up.
            co_await stream.read(std::string(256, '\0'));
        }(std::move(listener))).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19891, "test-client");
            std::string payload = "hello mqtt";
            co_await client.publish(
                "test/topic",
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(payload.data()), payload.size()),
                /*qos=*/0);
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "MqttClient::publish threw unexpectedly";
}

TEST_F(MqttLoopback, SubscribeAndReceive) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;
    std::string received_topic;
    std::string received_payload;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught,
                std::string& received_topic, std::string& received_payload) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19892);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();

            auto [n1, conn_pkt] = co_await stream.read(std::string(256, '\0')); // CONNECT
            (void)n1;
            co_await stream.write(build_connack());

            auto [n2, sub_pkt] = co_await stream.read(std::string(256, '\0')); // SUBSCRIBE
            sub_pkt.resize(n2);
            uint16_t pkt_id = extract_subscribe_pkt_id(sub_pkt);
            co_await stream.write(build_suback(pkt_id, /*granted_qos=*/0));

            // Scripted: deliver one message right after the subscription is acked.
            co_await stream.write(build_publish_qos0("test/topic", "hello mqtt"));
        }(std::move(listener))).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19892, "test-client");
            auto rx = co_await client.subscribe("test/topic");
            auto msg = co_await rx.recv();
            EXPECT_TRUE(msg.has_value());
            if (msg.has_value()) {
                received_topic = msg->topic;
                received_payload = std::string(
                    reinterpret_cast<const char*>(msg->payload.data()), msg->payload.size());
            }
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught, received_topic, received_payload)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "subscribe/receive path threw unexpectedly";
    EXPECT_EQ(received_topic, "test/topic");
    EXPECT_EQ(received_payload, "hello mqtt");
}

TEST_F(MqttLoopback, SubscribeRefused) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19893);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();

            auto [n1, conn_pkt] = co_await stream.read(std::string(256, '\0')); // CONNECT
            (void)n1;
            co_await stream.write(build_connack());

            auto [n2, sub_pkt] = co_await stream.read(std::string(256, '\0')); // SUBSCRIBE
            sub_pkt.resize(n2);
            uint16_t pkt_id = extract_subscribe_pkt_id(sub_pkt);
            // 0x80 == SUBACK failure code — lwIP maps any code >= 0x03 to ERR_ABRT.
            co_await stream.write(build_suback(pkt_id, /*granted_qos=*/0x80));
        }(std::move(listener))).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19893, "test-client");
            auto rx = co_await client.subscribe("test/topic");
            (void)rx;
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    ASSERT_NE(caught, nullptr) << "subscribe() should throw when the broker refuses the subscription";
    EXPECT_THROW(std::rethrow_exception(caught), std::runtime_error);
}

TEST_F(MqttLoopback, SubscribeIgnoresNonMatchingTopic) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;
    std::string received_topic;
    std::string received_payload;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught,
                std::string& received_topic, std::string& received_payload) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19894);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();

            auto [n1, conn_pkt] = co_await stream.read(std::string(256, '\0')); // CONNECT
            (void)n1;
            co_await stream.write(build_connack());

            auto [n2, sub_pkt] = co_await stream.read(std::string(256, '\0')); // SUBSCRIBE
            sub_pkt.resize(n2);
            uint16_t pkt_id = extract_subscribe_pkt_id(sub_pkt);
            co_await stream.write(build_suback(pkt_id, /*granted_qos=*/0));

            // Scripted: a message on a different topic must be dropped by the
            // client's topic filter, then the matching one must still arrive.
            co_await stream.write(build_publish_qos0("other/topic", "should be dropped"));
            co_await stream.write(build_publish_qos0("test/topic", "hello mqtt"));
        }(std::move(listener))).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19894, "test-client");
            auto rx = co_await client.subscribe("test/topic");
            auto msg = co_await rx.recv();
            EXPECT_TRUE(msg.has_value());
            if (msg.has_value()) {
                received_topic = msg->topic;
                received_payload = std::string(
                    reinterpret_cast<const char*>(msg->payload.data()), msg->payload.size());
            }
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught, received_topic, received_payload)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "subscribe/receive path threw unexpectedly";
    EXPECT_EQ(received_topic, "test/topic");
    EXPECT_EQ(received_payload, "hello mqtt");
}

TEST_F(MqttLoopback, SubscribeDeliversMultipleMessages) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;
    std::vector<std::string> received_payloads;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught,
                std::vector<std::string>& received_payloads) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19895);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();

            auto [n1, conn_pkt] = co_await stream.read(std::string(256, '\0')); // CONNECT
            (void)n1;
            co_await stream.write(build_connack());

            auto [n2, sub_pkt] = co_await stream.read(std::string(256, '\0')); // SUBSCRIBE
            sub_pkt.resize(n2);
            uint16_t pkt_id = extract_subscribe_pkt_id(sub_pkt);
            co_await stream.write(build_suback(pkt_id, /*granted_qos=*/0));

            co_await stream.write(build_publish_qos0("test/topic", "first"));
            co_await stream.write(build_publish_qos0("test/topic", "second"));
            co_await stream.write(build_publish_qos0("test/topic", "third"));
        }(std::move(listener))).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19895, "test-client");
            auto rx = co_await client.subscribe("test/topic");
            for (int i = 0; i < 3; ++i) {
                auto msg = co_await rx.recv();
                EXPECT_TRUE(msg.has_value());
                if (msg.has_value()) {
                    received_payloads.emplace_back(
                        reinterpret_cast<const char*>(msg->payload.data()), msg->payload.size());
                }
            }
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught, received_payloads)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "subscribe/receive path threw unexpectedly";
    ASSERT_EQ(received_payloads.size(), 3u);
    EXPECT_EQ(received_payloads[0], "first");
    EXPECT_EQ(received_payloads[1], "second");
    EXPECT_EQ(received_payloads[2], "third");
}

TEST_F(MqttLoopback, ConcurrentSubscriptionsToDistinctTopicsAreIndependent) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;
    std::string received_topic_a;
    std::string received_payload_a;
    std::string received_topic_b;
    std::string received_payload_b;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught,
                std::string& received_topic_a, std::string& received_payload_a,
                std::string& received_topic_b, std::string& received_payload_b) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19896);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();

            auto [n1, conn_pkt] = co_await stream.read(std::string(256, '\0')); // CONNECT
            (void)n1;
            co_await stream.write(build_connack());

            auto [n2, sub_pkt_a] = co_await stream.read(std::string(256, '\0')); // SUBSCRIBE topicA
            sub_pkt_a.resize(n2);
            uint16_t pkt_id_a = extract_subscribe_pkt_id(sub_pkt_a);
            co_await stream.write(build_suback(pkt_id_a, /*granted_qos=*/0));

            auto [n3, sub_pkt_b] = co_await stream.read(std::string(256, '\0')); // SUBSCRIBE topicB
            sub_pkt_b.resize(n3);
            uint16_t pkt_id_b = extract_subscribe_pkt_id(sub_pkt_b);
            co_await stream.write(build_suback(pkt_id_b, /*granted_qos=*/0));

            // Both subscriptions are live by this point — interleave the two
            // topics' messages to prove neither channel sees the other's traffic.
            co_await stream.write(build_publish_qos0("topicA", "first"));
            co_await stream.write(build_publish_qos0("topicB", "second"));
        }(std::move(listener))).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19896, "test-client");

            // Both subscriptions must be established before the broker script above
            // sends either message — otherwise an early PUBLISH for a topic without
            // a channel entry yet would be silently dropped (see on_incoming_data()).
            auto rx_a = co_await client.subscribe("topicA");
            auto rx_b = co_await client.subscribe("topicB");

            auto msg_b = co_await rx_b.recv();
            EXPECT_TRUE(msg_b.has_value());
            if (msg_b.has_value()) {
                received_topic_b = msg_b->topic;
                received_payload_b = std::string(
                    reinterpret_cast<const char*>(msg_b->payload.data()), msg_b->payload.size());
            }

            // topicA's message arrived first on the wire but is read second here —
            // it must still be sitting in topicA's own channel, untouched by topicB's
            // traffic, proving the two channels are isolated from each other.
            auto msg_a = co_await rx_a.recv();
            EXPECT_TRUE(msg_a.has_value());
            if (msg_a.has_value()) {
                received_topic_a = msg_a->topic;
                received_payload_a = std::string(
                    reinterpret_cast<const char*>(msg_a->payload.data()), msg_a->payload.size());
            }
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught,
      received_topic_a, received_payload_a, received_topic_b, received_payload_b)).detach();

    poll_until(rt, done);
    EXPECT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "concurrent subscription path threw unexpectedly";
    EXPECT_EQ(received_topic_a, "topicA");
    EXPECT_EQ(received_payload_a, "first");
    EXPECT_EQ(received_topic_b, "topicB");
    EXPECT_EQ(received_payload_b, "second");
}

TEST_F(MqttLoopback, SecondSubscribeToSameTopicGetsIndependentReceiverOnSameChannel) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;
    std::string payload1;
    std::string payload2;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught,
                std::string& payload1, std::string& payload2) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19899);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();

            auto [n1, conn_pkt] = co_await stream.read(std::string(256, '\0')); // CONNECT
            (void)n1;
            co_await stream.write(build_connack());

            // lwIP's mqtt_subscribe() re-sends SUBSCRIBE on the wire even for a topic
            // already subscribed to — MqttCtx's channel map only affects local fan-out.
            auto [n2, sub_pkt_1] = co_await stream.read(std::string(256, '\0'));
            sub_pkt_1.resize(n2);
            co_await stream.write(build_suback(extract_subscribe_pkt_id(sub_pkt_1), 0));

            auto [n3, sub_pkt_2] = co_await stream.read(std::string(256, '\0'));
            sub_pkt_2.resize(n3);
            co_await stream.write(build_suback(extract_subscribe_pkt_id(sub_pkt_2), 0));

            co_await stream.write(build_publish_qos0("shared/topic", "broadcast"));
        }(std::move(listener))).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19899, "test-client");
            auto rx1 = co_await client.subscribe("shared/topic");
            auto rx2 = co_await client.subscribe("shared/topic");

            // Both receivers were created before the single PUBLISH above was sent,
            // so both must independently observe it — that's the point of broadcast
            // fan-out vs. a single-consumer channel.
            auto msg1 = co_await rx1.recv();
            auto msg2 = co_await rx2.recv();
            EXPECT_TRUE(msg1.has_value());
            EXPECT_TRUE(msg2.has_value());
            if (msg1.has_value())
                payload1 = std::string(
                    reinterpret_cast<const char*>(msg1->payload.data()), msg1->payload.size());
            if (msg2.has_value())
                payload2 = std::string(
                    reinterpret_cast<const char*>(msg2->payload.data()), msg2->payload.size());
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught, payload1, payload2)).detach();

    poll_until(rt, done);
    EXPECT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "shared-topic subscription path threw unexpectedly";
    EXPECT_EQ(payload1, "broadcast");
    EXPECT_EQ(payload2, "broadcast");
}

TEST_F(MqttLoopback, PublishQos1WaitsForPuback) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19897);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();

            auto [n1, conn_pkt] = co_await stream.read(std::string(256, '\0')); // CONNECT
            (void)n1;
            co_await stream.write(build_connack());

            auto [n2, pub_pkt] = co_await stream.read(std::string(256, '\0')); // PUBLISH QoS1
            pub_pkt.resize(n2);
            uint16_t pkt_id = extract_publish_pkt_id_qos1(pub_pkt);
            co_await stream.write(build_puback(pkt_id));
        }(std::move(listener))).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19897, "test-client");
            std::string payload = "hello qos1";
            co_await client.publish(
                "test/topic",
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(payload.data()), payload.size()),
                /*qos=*/1);
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "MqttClient::publish (QoS1) threw unexpectedly";
}

TEST_F(MqttLoopback, PublishThrowsAfterServerDisconnect) {
    coro::Runtime rt;
    bool connected_done = false;
    std::optional<coro::MqttClient> client_opt;

    rt.spawn([](coro::Runtime& rt, bool& connected_done,
                std::optional<coro::MqttClient>& client_opt) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19898);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();
            auto [n, buf] = co_await stream.read(std::string(256, '\0')); // CONNECT
            (void)n;
            co_await stream.write(build_connack());
            // `stream` closes the TCP connection on destruction here, which
            // triggers lwIP's second on_connect callback (MQTT_CONNECT_DISCONNECTED).
        }(std::move(listener))).detach();

        auto client = co_await coro::MqttClient::connect("127.0.0.1", 19898, "test-client");
        client_opt = std::move(client);
        connected_done = true;
    }(rt, connected_done, client_opt)).detach();

    poll_until(rt, connected_done);
    ASSERT_TRUE(connected_done) << "test timed out waiting for connect";

    // Drive extra polling iterations so lwIP observes the broker's TCP close
    // and fires the disconnect status callback before we attempt to publish.
    for (int i = 0; i < 1000; ++i) {
        rt.poll();
        sys_check_timeouts();
        netif_poll_all();
    }

    bool publish_done = false;
    std::exception_ptr caught;

    rt.spawn([](bool& publish_done, std::exception_ptr& caught,
                std::optional<coro::MqttClient>& client_opt) -> coro::Coro<void> {
        try {
            std::string payload = "x";
            co_await client_opt->publish(
                "test/topic",
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(payload.data()), payload.size()),
                /*qos=*/0);
        } catch (...) {
            caught = std::current_exception();
        }
        publish_done = true;
    }(publish_done, caught, client_opt)).detach();

    poll_until(rt, publish_done);
    ASSERT_TRUE(publish_done) << "test timed out waiting for publish";
    ASSERT_NE(caught, nullptr) << "publish() should throw once the broker connection has dropped";
    EXPECT_THROW(std::rethrow_exception(caught), std::runtime_error);
}

TEST_F(MqttLoopback, ConnectWithLastWillSetsWillFlagAndFields) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;
    ParsedConnect parsed;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught,
                ParsedConnect& parsed) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19900);

        rt.spawn([](coro::TcpListener l, ParsedConnect& parsed) -> coro::Coro<void> {
            auto stream = co_await l.accept();
            auto [n, buf] = co_await stream.read(std::string(256, '\0')); // CONNECT
            buf.resize(n);
            parsed = parse_connect(buf);
            co_await stream.write(build_connack());
        }(std::move(listener), parsed)).detach();

        try {
            auto client = co_await coro::MqttClient::connect(
                "127.0.0.1", 19900, "test-client",
                "kitchen/led/status", "offline", /*will_qos=*/1, /*will_retain=*/false);
            (void)client;
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught, parsed)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "MqttClient::connect with LWT threw unexpectedly";
    EXPECT_TRUE(parsed.flags & 0x04) << "will flag should be set in CONNECT flags byte";
    ASSERT_TRUE(parsed.will_topic.has_value());
    ASSERT_TRUE(parsed.will_message.has_value());
    EXPECT_EQ(*parsed.will_topic, "kitchen/led/status");
    EXPECT_EQ(*parsed.will_message, "offline");
}

TEST_F(MqttLoopback, ConnectWithoutLastWillLeavesWillFlagClear) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;
    ParsedConnect parsed;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught,
                ParsedConnect& parsed) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19901);

        rt.spawn([](coro::TcpListener l, ParsedConnect& parsed) -> coro::Coro<void> {
            auto stream = co_await l.accept();
            auto [n, buf] = co_await stream.read(std::string(256, '\0')); // CONNECT
            buf.resize(n);
            parsed = parse_connect(buf);
            co_await stream.write(build_connack());
        }(std::move(listener), parsed)).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19901, "test-client");
            (void)client;
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught, parsed)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "MqttClient::connect threw unexpectedly";
    EXPECT_FALSE(parsed.flags & 0x04) << "will flag should be clear when no LWT is given";
    EXPECT_FALSE(parsed.will_topic.has_value());
}

TEST_F(MqttLoopback, ConnectWithCredentialsSetsUsernameAndPasswordFields) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;
    ParsedConnect parsed;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught,
                ParsedConnect& parsed) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19902);

        rt.spawn([](coro::TcpListener l, ParsedConnect& parsed) -> coro::Coro<void> {
            auto stream = co_await l.accept();
            auto [n, buf] = co_await stream.read(std::string(256, '\0')); // CONNECT
            buf.resize(n);
            parsed = parse_connect(buf);
            co_await stream.write(build_connack());
        }(std::move(listener), parsed)).detach();

        try {
            auto client = co_await coro::MqttClient::connect(
                "127.0.0.1", 19902, "test-client",
                /*will_topic=*/{}, /*will_payload=*/{}, /*will_qos=*/0, /*will_retain=*/false,
                "bruno", "NoCloudSky");
            (void)client;
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught, parsed)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "MqttClient::connect with credentials threw unexpectedly";
    EXPECT_TRUE(parsed.flags & 0x80) << "user name flag should be set when username is given";
    EXPECT_TRUE(parsed.flags & 0x40) << "password flag should be set when password is given";
    ASSERT_TRUE(parsed.username.has_value());
    ASSERT_TRUE(parsed.password.has_value());
    EXPECT_EQ(*parsed.username, "bruno");
    EXPECT_EQ(*parsed.password, "NoCloudSky");
}

TEST_F(MqttLoopback, ConnectWithoutCredentialsLeavesUsernameFlagsClear) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;
    ParsedConnect parsed;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught,
                ParsedConnect& parsed) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19903);

        rt.spawn([](coro::TcpListener l, ParsedConnect& parsed) -> coro::Coro<void> {
            auto stream = co_await l.accept();
            auto [n, buf] = co_await stream.read(std::string(256, '\0')); // CONNECT
            buf.resize(n);
            parsed = parse_connect(buf);
            co_await stream.write(build_connack());
        }(std::move(listener), parsed)).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19903, "test-client");
            (void)client;
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught, parsed)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "MqttClient::connect threw unexpectedly";
    EXPECT_FALSE(parsed.flags & 0x80) << "user name flag should be clear when no username is given";
    EXPECT_FALSE(parsed.flags & 0x40) << "password flag should be clear when no password is given";
    EXPECT_FALSE(parsed.username.has_value());
    EXPECT_FALSE(parsed.password.has_value());
}
