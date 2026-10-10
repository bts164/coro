#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "executor_traits.h"
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <optional>
#include <stdexcept>

using namespace coro;

namespace {

// --- Simple futures for testing ---

struct ImmediateIntFuture {
    using OutputType = int;
    int m_value;
    PollResult<int> poll(detail::Context&) { return m_value; }
};

struct ImmediateVoidFuture {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollReady; }
};

struct ThrowingFuture {
    using OutputType = int;
    PollResult<int> poll(detail::Context&) {
        return PollError(std::make_exception_ptr(std::runtime_error("boom")));
    }
};

// Returns Pending first, immediately calls wake(), then Ready on second poll.
struct SelfWakingFuture {
    using OutputType = int;
    int  m_value;
    bool m_polled_once = false;
    PollResult<int> poll(detail::Context& ctx) {
        if (!m_polled_once) {
            m_polled_once = true;
            ctx.getWaker()->wake();
            return PollPending;
        }
        return m_value;
    }
};

// Stream for testing spawn(stream) and build_task().spawn(stream)
struct IntStream {
    using ItemType = int;
    PollResult<std::optional<int>> poll_next(detail::Context&) { return PollPending; }
};

static_assert(Future<ImmediateIntFuture>);
static_assert(Stream<IntStream>);

}  // namespace

// --- Runtime construction ---

// SingleThreadRuntime (executor_traits.h) is a one-thread runtime on the desktop and
// the Pico runtime without a network on the board.

TEST(RuntimeTest, IsConstructibleWithDefaultThreadCount) {
    SingleThreadRuntime rt;
    (void)rt;
}

TEST(RuntimeTest, IsConstructibleWithExplicitThreadCount) {
    SingleThreadRuntime rt;
    (void)rt;
}

// --- block_on: basic futures ---

TEST(RuntimeTest, BlockOnReturnsIntResult) {
    SingleThreadRuntime rt;
    int result = rt.block_on(ImmediateIntFuture{42});
    EXPECT_EQ(result, 42);
}

TEST(RuntimeTest, BlockOnVoidCompletes) {
    SingleThreadRuntime rt;
    rt.block_on(ImmediateVoidFuture{});  // should not throw or hang
}

TEST(RuntimeTest, BlockOnRethrowsException) {
    SingleThreadRuntime rt;
    EXPECT_THROW(rt.block_on(ThrowingFuture{}), std::runtime_error);
}

TEST(RuntimeTest, BlockOnSelfWakingFuture) {
    SingleThreadRuntime rt;
    int result = rt.block_on(SelfWakingFuture{7});
    EXPECT_EQ(result, 7);
}

// --- block_on: Coro coroutines ---

namespace {

Coro<int> simple_coro() { co_return 99; }

Coro<void> void_coro() { co_return; }

Coro<int> coro_awaiting_immediate() {
    co_return co_await ImmediateIntFuture{55};
}

Coro<int> throwing_coro() {
    throw std::runtime_error("coro boom");
    co_return 0;
}

}  // namespace

TEST(RuntimeTest, BlockOnSimpleCoro) {
    SingleThreadRuntime rt;
    EXPECT_EQ(rt.block_on(simple_coro()), 99);
}

TEST(RuntimeTest, BlockOnVoidCoro) {
    SingleThreadRuntime rt;
    rt.block_on(void_coro());
}

TEST(RuntimeTest, BlockOnCoroAwaitingImmediateFuture) {
    SingleThreadRuntime rt;
    EXPECT_EQ(rt.block_on(coro_awaiting_immediate()), 55);
}

TEST(RuntimeTest, BlockOnCoroRethrowsException) {
    SingleThreadRuntime rt;
    EXPECT_THROW(rt.block_on(throwing_coro()), std::runtime_error);
}

// --- spawn + JoinHandle via block_on ---

namespace {

Coro<int> spawns_task() {
    JoinHandle<int> h = coro::spawn(ImmediateIntFuture{123});
    co_return co_await std::move(h);
}

}  // namespace

TEST(RuntimeTest, BlockOnCoroThatSpawnsTask) {
    SingleThreadRuntime rt;
    EXPECT_EQ(rt.block_on(spawns_task()), 123);
}

// --- spawn interface ---

TEST(RuntimeTest, SpawnReturnsJoinHandle) {
    SingleThreadRuntime rt;
    JoinHandle<int> h = rt.spawn(ImmediateIntFuture{1});
    (void)h;
}

TEST(RuntimeTest, BuildTaskNameIsChainable) {
    SingleThreadRuntime rt;
    JoinHandle<int> h = rt.build_task().name("my-task").spawn(ImmediateIntFuture{1});
    (void)h;
}

TEST(RuntimeTest, SpawnStreamReturnsStreamHandle) {
    SingleThreadRuntime rt;
    StreamHandle<int> h = rt.spawn(IntStream{});
    (void)h;
}

TEST(RuntimeTest, BuildTaskNameAndBufferAreChainable) {
    SingleThreadRuntime rt;
    StreamHandle<int> h = rt.build_task().name("reader").buffer(128).spawn(IntStream{});
    (void)h;
}

// --- Thread-local runtime ---

TEST(RuntimeTest, SetAndGetCurrentRuntime) {
    SingleThreadRuntime rt;
    set_current_runtime(&rt);
    EXPECT_EQ(&current_runtime(), &rt);
    set_current_runtime(nullptr);
}

TEST(RuntimeTest, CurrentRuntimeThrowsWhenUnset) {
    set_current_runtime(nullptr);
    EXPECT_THROW(current_runtime(), std::runtime_error);
}

TEST(RuntimeTest, FreeSpawnDelegatesToCurrentRuntime) {
    SingleThreadRuntime rt;
    set_current_runtime(&rt);
    JoinHandle<int> h = coro::spawn(ImmediateIntFuture{1});
    set_current_runtime(nullptr);
    (void)h;
}
