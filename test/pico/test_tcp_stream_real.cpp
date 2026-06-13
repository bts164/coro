#include <coro/io/tcp_stream.h>
#include <coro/io/tcp_listener.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <gtest/gtest.h>

#include <lwip/init.h>
#include <lwip/timeouts.h>
#include <lwip/netif.h>

// ---------------------------------------------------------------------------
// Fixture — initialises lwIP once per process.
//
// lwip_init() automatically creates a loopback netif at 127.0.0.1 when
// LWIP_HAVE_LOOPIF=1 (set in lwipopts.h). No manual netif_add() needed.
// ---------------------------------------------------------------------------

class LwipLoopback : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        lwip_init();
    }

    // Drive the PicoExecutor and lwIP together until flag is set.
    // netif_poll_all() delivers queued loopback packets each iteration.
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

TEST_F(LwipLoopback, EchoLoopback) {
    coro::Runtime rt;
    bool done = false;
    std::string received;

    // Pass rt explicitly so inner rt.spawn() doesn't need current_runtime().
    // coro::spawn() calls current_runtime() which is only set inside block_on();
    // our manual poll loop never sets it, so use rt.spawn() throughout.
    rt.spawn([](coro::Runtime& rt, bool& done, std::string& received) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19877);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();
            auto [n, buf] = co_await stream.read(std::string(256, '\0'));
            buf.resize(n);
            co_await stream.write(std::move(buf));
        }(std::move(listener))).detach();

        auto client = co_await coro::TcpStream::connect("127.0.0.1", 19877);
        co_await client.write(std::string("hello loopback"));
        auto [n, reply] = co_await client.read(std::string(256, '\0'));
        reply.resize(n);
        received = reply;
        done = true;
    }(rt, done, received)).detach();

    poll_until(rt, done);

    ASSERT_TRUE(done) << "test timed out — poll loop exhausted without completing";
    EXPECT_EQ(received, "hello loopback");
}

TEST_F(LwipLoopback, MultipleMessages) {
    coro::Runtime rt;
    bool done = false;

    rt.spawn([](coro::Runtime& rt, bool& done) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19878);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();
            for (int i = 0; i < 3; ++i) {
                auto [n, buf] = co_await stream.read(std::string(64, '\0'));
                buf.resize(n);
                co_await stream.write(std::move(buf));
            }
        }(std::move(listener))).detach();

        auto client = co_await coro::TcpStream::connect("127.0.0.1", 19878);
        for (std::string msg : {"one", "two", "three"}) {
            co_await client.write(std::string(msg));
            auto [n, reply] = co_await client.read(std::string(64, '\0'));
            reply.resize(n);
            EXPECT_EQ(reply, msg);
        }
        done = true;
    }(rt, done)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
}

TEST_F(LwipLoopback, ConnectionRefused) {
    coro::Runtime rt;
    bool done = false;
    std::exception_ptr caught;

    rt.spawn([](coro::Runtime&, bool& done, std::exception_ptr& caught) -> coro::Coro<void> {
        try {
            // Port 19999 has no listener — lwIP loopback returns RST.
            auto stream = co_await coro::TcpStream::connect("127.0.0.1", 19999);
            (void)stream;
        } catch (...) {
            caught = std::current_exception();
        }
        done = true;
    }(rt, done, caught)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
    ASSERT_NE(caught, nullptr) << "expected connect to throw but no exception was thrown";
    try {
        std::rethrow_exception(caught);
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("connect"), std::string::npos);
    } catch (...) {
        FAIL() << "expected std::runtime_error";
    }
}

TEST_F(LwipLoopback, LargeDataTransfer) {
    // Exercises the write_impl flow-control loop: total data (32 KB) exceeds
    // TCP_SND_BUF (4 * TCP_MSS ≈ 5840 bytes), so write must chunk and wait for
    // on_sent callbacks to free send-buffer space before continuing.
    static constexpr std::size_t DATA_SIZE = 32 * 1024;

    coro::Runtime rt;
    bool done = false;

    rt.spawn([](coro::Runtime& rt, bool& done) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19879);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();
            // Read exactly DATA_SIZE bytes, then echo the whole thing back.
            std::string accumulated;
            while (accumulated.size() < DATA_SIZE) {
                std::size_t want = std::min(DATA_SIZE - accumulated.size(), std::size_t(8192));
                auto [n, buf] = co_await stream.read(std::string(want, '\0'));
                if (n == 0) break;
                buf.resize(n);
                accumulated += buf;
            }
            co_await stream.write(std::move(accumulated));
        }(std::move(listener))).detach();

        // Generate DATA_SIZE bytes of predictable content.
        std::string data(DATA_SIZE, '\0');
        for (std::size_t i = 0; i < DATA_SIZE; ++i)
            data[i] = static_cast<char>('A' + (i % 26));

        auto client = co_await coro::TcpStream::connect("127.0.0.1", 19879);
        co_await client.write(std::string(data));

        std::string received;
        while (received.size() < DATA_SIZE) {
            auto [n, buf] = co_await client.read(std::string(8192, '\0'));
            if (n == 0) break;
            buf.resize(n);
            received += buf;
        }
        EXPECT_EQ(received, data);
        done = true;
    }(rt, done)).detach();

    poll_until(rt, done, 1'000'000);
    ASSERT_TRUE(done) << "test timed out";
}

TEST_F(LwipLoopback, PartialRead) {
    // Verifies that read_impl returns min(requested, available) and leaves
    // the remainder in rx_buf for the next call.
    coro::Runtime rt;
    bool done = false;

    rt.spawn([](coro::Runtime& rt, bool& done) -> coro::Coro<void> {
        auto listener = co_await coro::TcpListener::bind("127.0.0.1", 19880);

        rt.spawn([](coro::TcpListener l) -> coro::Coro<void> {
            auto stream = co_await l.accept();
            co_await stream.write(std::string("hello world"));
        }(std::move(listener))).detach();

        auto client = co_await coro::TcpStream::connect("127.0.0.1", 19880);

        auto [n1, buf1] = co_await client.read(std::string(5, '\0'));
        buf1.resize(n1);
        EXPECT_EQ(buf1, "hello");

        auto [n2, buf2] = co_await client.read(std::string(32, '\0'));
        buf2.resize(n2);
        EXPECT_EQ(buf2, " world");

        done = true;
    }(rt, done)).detach();

    poll_until(rt, done);
    ASSERT_TRUE(done) << "test timed out";
}
