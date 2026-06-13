#include <coro/io/tcp_stream.h>
#include <coro/io/tcp_listener.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <gtest/gtest.h>
#include <type_traits>

// ---------------------------------------------------------------------------
// Compile-time API checks
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<coro::TcpStream>,
              "TcpStream must be move-only");
static_assert(std::is_move_constructible_v<coro::TcpStream>,
              "TcpStream must be movable");
static_assert(!std::is_copy_constructible_v<coro::TcpListener>,
              "TcpListener must be move-only");
static_assert(std::is_move_constructible_v<coro::TcpListener>,
              "TcpListener must be movable");

TEST(TcpStream, TypeProperties) {
    // Covered by the static_asserts above; this test exists so the suite has
    // at least one passing runtime test while the network infrastructure is
    // being set up.
    SUCCEED();
}

// ---------------------------------------------------------------------------
// TODO: enable the following tests once host lwIP NO_SYS infrastructure is set up.
//
// The test pattern is:
//   1. Call sys_check_timeouts() and lwip_init() in a fixture SetUp().
//   2. Spawn a TcpListener::bind() + accept() server coroutine.
//   3. Spawn a TcpStream::connect() + read/write client coroutine.
//   4. Drive both with rt.poll() + sys_check_timeouts() in a loop until done.
//
// Example skeleton:
//
// TEST(TcpStream, EchoLoopback) {
//     coro::Runtime rt;
//     bool done = false;
//
//     rt.spawn([](bool& done) -> coro::Coro<void> {
//         auto listener = co_await coro::TcpListener::bind("127.0.0.1", 9877);
//         auto server   = coro::spawn([](coro::TcpListener l) -> coro::Coro<void> {
//             auto stream = co_await l.accept();
//             auto [n, buf] = co_await stream.read(std::string(64, '\0'));
//             buf.resize(n);
//             co_await stream.write(std::move(buf));
//         }(std::move(listener)));
//
//         auto client = co_await coro::TcpStream::connect("127.0.0.1", 9877);
//         co_await client.write(std::string("hello"));
//         auto [n, reply] = co_await client.read(std::string(64, '\0'));
//         reply.resize(n);
//         EXPECT_EQ(reply, "hello");
//         done = true;
//     }(done)).detach();
//
//     while (!done) {
//         rt.poll();
//         sys_check_timeouts();
//     }
// }
// ---------------------------------------------------------------------------
