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
            auto stream = client.subscribe("test/topic");
            auto msg = co_await coro::next(stream);
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
            auto stream = client.subscribe("test/topic");
            auto msg = co_await coro::next(stream);
            (void)msg;
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
            auto stream = client.subscribe("test/topic");
            auto msg = co_await coro::next(stream);
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
            auto stream = client.subscribe("test/topic");
            for (int i = 0; i < 3; ++i) {
                auto msg = co_await coro::next(stream);
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

TEST_F(MqttLoopback, SubscribeDisplacesPreviousSubscription) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;
    bool stream_a_closed_after_displacement = false;
    std::string received_topic_a;
    std::string received_payload_a;
    std::string received_topic_b;
    std::string received_payload_b;

    rt.spawn([](coro::Runtime& rt, bool& done, std::exception_ptr& caught,
                bool& stream_a_closed_after_displacement,
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
            co_await stream.write(build_publish_qos0("topicA", "first"));

            auto [n3, sub_pkt_b] = co_await stream.read(std::string(256, '\0')); // SUBSCRIBE topicB
            sub_pkt_b.resize(n3);
            uint16_t pkt_id_b = extract_subscribe_pkt_id(sub_pkt_b);
            co_await stream.write(build_suback(pkt_id_b, /*granted_qos=*/0));
            co_await stream.write(build_publish_qos0("topicB", "second"));
        }(std::move(listener))).detach();

        try {
            auto client = co_await coro::MqttClient::connect("127.0.0.1", 19896, "test-client");

            auto stream_a = client.subscribe("topicA");
            auto msg_a1 = co_await coro::next(stream_a);
            EXPECT_TRUE(msg_a1.has_value());
            if (msg_a1.has_value()) {
                received_topic_a = msg_a1->topic;
                received_payload_a = std::string(
                    reinterpret_cast<const char*>(msg_a1->payload.data()), msg_a1->payload.size());
            }

            // Subscribing to topicB displaces topicA's channel — see the
            // single-consumer note on MqttClient::subscribe().
            auto stream_b = client.subscribe("topicB");
            auto msg_b = co_await coro::next(stream_b);
            EXPECT_TRUE(msg_b.has_value());
            if (msg_b.has_value()) {
                received_topic_b = msg_b->topic;
                received_payload_b = std::string(
                    reinterpret_cast<const char*>(msg_b->payload.data()), msg_b->payload.size());
            }

            // stream_a's channel sender was dropped when stream_b displaced
            // it — the stream must end (nullopt), not hang.
            auto msg_a2 = co_await coro::next(stream_a);
            stream_a_closed_after_displacement = !msg_a2.has_value();
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught, stream_a_closed_after_displacement,
      received_topic_a, received_payload_a, received_topic_b, received_payload_b)).detach();

    poll_until(rt, done);
    EXPECT_TRUE(done) << "test timed out";
    EXPECT_EQ(caught, nullptr) << "subscribe displacement path threw unexpectedly";
    EXPECT_EQ(received_topic_a, "topicA");
    EXPECT_EQ(received_payload_a, "first");
    EXPECT_EQ(received_topic_b, "topicB");
    EXPECT_EQ(received_payload_b, "second");
    EXPECT_TRUE(stream_a_closed_after_displacement)
        << "displaced subscription's stream should end, not hang";
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
