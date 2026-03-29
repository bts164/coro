#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <coro/runtime.h>
#include <coro/coro.h>
#include <stdexcept>

using namespace coro;

// --- Simple futures for testing ---

struct ImmediateIntFuture {
    using OutputType = int;
    int m_value;
    PollResult<int> poll(Context&) { return m_value; }
};

struct ImmediateVoidFuture {
    using OutputType = void;
    PollResult<void> poll(Context&) { return PollReady; }
};

struct ThrowingFuture {
    using OutputType = int;
    PollResult<int> poll(Context&) {
        return PollError(std::make_exception_ptr(std::runtime_error("boom")));
    }
};

// Returns Pending first, immediately calls wake(), then Ready on second poll.
struct SelfWakingFuture {
    using OutputType = int;
    int  m_value;
    bool m_polled_once = false;
    PollResult<int> poll(Context& ctx) {
        if (!m_polled_once) {
            m_polled_once = true;
            ctx.getWaker()->wake();
            return PollPending;
        }
        return m_value;
    }
};

// Stream for testing StreamSpawnBuilder
struct IntStream {
    using ItemType = int;
    PollResult<std::optional<int>> poll_next(Context&) { return PollPending; }
};

static_assert(Future<ImmediateIntFuture>);
static_assert(Stream<IntStream>);

// --- Runtime construction ---

TEST(RuntimeTest, IsConstructibleWithDefaultThreadCount) {
    Runtime rt;
    (void)rt;
}

TEST(RuntimeTest, IsConstructibleWithExplicitThreadCount) {
    Runtime rt(1);
    (void)rt;
}

// --- block_on: basic futures ---

TEST(RuntimeTest, BlockOnReturnsIntResult) {
    Runtime rt;
    int result = rt.block_on(ImmediateIntFuture{42});
    EXPECT_EQ(result, 42);
}

TEST(RuntimeTest, BlockOnVoidCompletes) {
    Runtime rt;
    rt.block_on(ImmediateVoidFuture{});  // should not throw or hang
}

TEST(RuntimeTest, BlockOnRethrowsException) {
    Runtime rt;
    EXPECT_THROW(rt.block_on(ThrowingFuture{}), std::runtime_error);
}

TEST(RuntimeTest, BlockOnSelfWakingFuture) {
    Runtime rt;
    int result = rt.block_on(SelfWakingFuture{7});
    EXPECT_EQ(result, 7);
}

// --- block_on: Coro coroutines ---

Coro<int> simple_coro() { co_return 99; }

Coro<void> void_coro() { co_return; }

Coro<int> coro_awaiting_immediate() {
    co_return co_await ImmediateIntFuture{55};
}

Coro<int> throwing_coro() {
    throw std::runtime_error("coro boom");
    co_return 0;
}

TEST(RuntimeTest, BlockOnSimpleCoro) {
    Runtime rt;
    EXPECT_EQ(rt.block_on(simple_coro()), 99);
}

TEST(RuntimeTest, BlockOnVoidCoro) {
    Runtime rt;
    rt.block_on(void_coro());
}

TEST(RuntimeTest, BlockOnCoroAwaitingImmediateFuture) {
    Runtime rt;
    EXPECT_EQ(rt.block_on(coro_awaiting_immediate()), 55);
}

TEST(RuntimeTest, BlockOnCoroRethrowsException) {
    Runtime rt;
    EXPECT_THROW(rt.block_on(throwing_coro()), std::runtime_error);
}

// --- spawn + JoinHandle via block_on ---

Coro<int> spawns_task() {
    JoinHandle<int> h = coro::spawn(ImmediateIntFuture{123}).submit();
    co_return co_await std::move(h);
}

TEST(RuntimeTest, BlockOnCoroThatSpawnsTask) {
    Runtime rt;
    EXPECT_EQ(rt.block_on(spawns_task()), 123);
}

// --- SpawnBuilder interface ---

TEST(RuntimeTest, SpawnBuilderSubmitReturnsJoinHandle) {
    Runtime rt;
    JoinHandle<int> h = rt.spawn(ImmediateIntFuture{1}).submit();
    (void)h;
}

TEST(RuntimeTest, SpawnBuilderNameIsChainable) {
    Runtime rt;
    JoinHandle<int> h = rt.spawn(ImmediateIntFuture{1}).name("my-task").submit();
    (void)h;
}

TEST(RuntimeTest, StreamBuilderSubmitReturnsStreamHandle) {
    Runtime rt;
    StreamHandle<int> h = rt.spawn(IntStream{}).submit();
    (void)h;
}

TEST(RuntimeTest, StreamBuilderNameAndBufferAreChainable) {
    Runtime rt;
    StreamHandle<int> h = rt.spawn(IntStream{}).name("reader").buffer(128).submit();
    (void)h;
}

// --- Thread-local runtime ---

TEST(RuntimeTest, SetAndGetCurrentRuntime) {
    Runtime rt;
    set_current_runtime(&rt);
    EXPECT_EQ(&current_runtime(), &rt);
    set_current_runtime(nullptr);
}

TEST(RuntimeTest, CurrentRuntimeThrowsWhenUnset) {
    set_current_runtime(nullptr);
    EXPECT_THROW(current_runtime(), std::runtime_error);
}

TEST(RuntimeTest, FreeSpawnDelegatesToCurrentRuntime) {
    Runtime rt;
    set_current_runtime(&rt);
    JoinHandle<int> h = coro::spawn(ImmediateIntFuture{1}).submit();
    set_current_runtime(nullptr);
    (void)h;
}
