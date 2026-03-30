#include <gtest/gtest.h>
#include <coro/detail/poll_result.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

using namespace coro;

// --- PollResult<int> ---

TEST(PollResultTest, PendingState) {
    PollResult<int> r = PollPending;
    EXPECT_TRUE(r.isPending());
    EXPECT_FALSE(r.isReady());
    EXPECT_FALSE(r.isError());
}

TEST(PollResultTest, ReadyState) {
    PollResult<int> r = 42;
    EXPECT_FALSE(r.isPending());
    EXPECT_TRUE(r.isReady());
    EXPECT_FALSE(r.isError());
    EXPECT_EQ(r.value(), 42);
}

TEST(PollResultTest, ReadyStateMoveOnly) {
    PollResult<std::unique_ptr<int>> r = std::make_unique<int>(99);
    EXPECT_TRUE(r.isReady());
    EXPECT_EQ(*std::move(r).value(), 99);
}

TEST(PollResultTest, ErrorState) {
    auto eptr = std::make_exception_ptr(std::runtime_error("oops"));
    PollResult<int> r = PollError(eptr);
    EXPECT_FALSE(r.isPending());
    EXPECT_FALSE(r.isReady());
    EXPECT_TRUE(r.isError());
    EXPECT_EQ(r.error(), eptr);
}

TEST(PollResultTest, RethrowIfError) {
    auto eptr = std::make_exception_ptr(std::runtime_error("oops"));
    PollResult<int> r = PollError(eptr);
    EXPECT_THROW(r.rethrowIfError(), std::runtime_error);
}

TEST(PollResultTest, RethrowIfErrorNoopWhenReady) {
    PollResult<int> r = 42;
    EXPECT_NO_THROW(r.rethrowIfError());
}

TEST(PollResultTest, RethrowIfErrorNoopWhenPending) {
    PollResult<int> r = PollPending;
    EXPECT_NO_THROW(r.rethrowIfError());
}

// --- PollResult<void> ---

TEST(PollResultVoidTest, PendingState) {
    PollResult<void> r = PollPending;
    EXPECT_TRUE(r.isPending());
    EXPECT_FALSE(r.isReady());
    EXPECT_FALSE(r.isError());
}

TEST(PollResultVoidTest, ReadyState) {
    PollResult<void> r = PollReady;
    EXPECT_FALSE(r.isPending());
    EXPECT_TRUE(r.isReady());
    EXPECT_FALSE(r.isError());
}

TEST(PollResultVoidTest, ErrorState) {
    auto eptr = std::make_exception_ptr(std::runtime_error("oops"));
    PollResult<void> r = PollError(eptr);
    EXPECT_TRUE(r.isError());
    EXPECT_THROW(r.rethrowIfError(), std::runtime_error);
}

// --- PollError converts to any PollResult<T> ---

TEST(PollResultTest, PollErrorConvertsToAnyType) {
    auto eptr = std::make_exception_ptr(std::runtime_error("x"));
    PollResult<int>         r1 = PollError(eptr);
    PollResult<std::string> r2 = PollError(eptr);
    PollResult<void>        r3 = PollError(eptr);
    EXPECT_TRUE(r1.isError());
    EXPECT_TRUE(r2.isError());
    EXPECT_TRUE(r3.isError());
}

// --- Stream use case: PollResult<std::optional<T>> ---

TEST(PollResultOptionalTest, ReadySome) {
    PollResult<std::optional<int>> r = std::optional<int>(7);
    EXPECT_TRUE(r.isReady());
    EXPECT_EQ(r.value(), 7);
}

TEST(PollResultOptionalTest, ReadyNulloptSignalsExhaustion) {
    PollResult<std::optional<int>> r = std::optional<int>(std::nullopt);
    EXPECT_TRUE(r.isReady());
    EXPECT_FALSE(r.value().has_value());
}

TEST(PollResultOptionalTest, PendingState) {
    PollResult<std::optional<int>> r = PollPending;
    EXPECT_TRUE(r.isPending());
}

// --- PollDropped ---

TEST(PollResultTest, DroppedState) {
    PollResult<int> r = PollDropped;
    EXPECT_FALSE(r.isPending());
    EXPECT_FALSE(r.isReady());
    EXPECT_FALSE(r.isError());
    EXPECT_TRUE(r.isDropped());
}

TEST(PollResultVoidTest, DroppedState) {
    PollResult<void> r = PollDropped;
    EXPECT_FALSE(r.isPending());
    EXPECT_FALSE(r.isReady());
    EXPECT_FALSE(r.isError());
    EXPECT_TRUE(r.isDropped());
}

TEST(PollResultOptionalTest, DroppedState) {
    PollResult<std::optional<int>> r = PollDropped;
    EXPECT_TRUE(r.isDropped());
    EXPECT_FALSE(r.isPending());
    EXPECT_FALSE(r.isReady());
}
