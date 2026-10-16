#include <gtest/gtest.h>
#include "co_assert.h"
#include "executor_traits.h"
#include <coro/task/parallel.h>
#include <coro/sync/select.h>
#include <coro/runtime/runtime.h>
#include <coro/coro.h>
#include <coro/future.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef CORO_PICO
#include <chrono>
#include <thread>
#endif

using namespace coro;

namespace {

constexpr std::size_t kMaxParts = 8;
using Seen = std::array<std::atomic<int>, kMaxParts>;

struct ReadyVoidFuture {
    using OutputType = void;
    PollResult<void> poll(detail::Context&) { return PollReady; }
};

// Counts the destruction of the coroutine frame it is passed into, whether or not
// the coroutine has started: a part cancelled before its first poll never runs its
// body.
class DropToken {
public:
    explicit DropToken(std::atomic<int>& dropped) : m_dropped(&dropped) {}
    DropToken(DropToken&& other) noexcept : m_dropped(std::exchange(other.m_dropped, nullptr)) {}
    ~DropToken() { if (m_dropped) m_dropped->fetch_add(1); }
private:
    std::atomic<int>* m_dropped;
};

// Counts its copies; moves are free.
struct CopyCounter {
    std::atomic<int>* copies;
    explicit CopyCounter(std::atomic<int>& c) : copies(&c) {}
    CopyCounter(const CopyCounter& other) : copies(other.copies) { copies->fetch_add(1); }
    CopyCounter(CopyCounter&&) noexcept = default;
};

Coro<void> record_index(TeamMember part, Seen& seen, std::atomic<bool>& bad_count,
                        std::size_t expected_count) {
    if (part.size() != expected_count) bad_count.store(true);
    seen[part.rank()].fetch_add(1);
    co_return;
}

// Each phase checks that every part finished the one before.
Coro<void> two_phases(TeamMember part, std::atomic<int>& phase1, std::atomic<int>& phase2,
                      std::atomic<bool>& out_of_step) {
    phase1.fetch_add(1);
    co_await part.sync();
    if (phase1.load() != static_cast<int>(part.size())) out_of_step.store(true);
    phase2.fetch_add(1);
    co_await part.sync();
    if (phase2.load() != static_cast<int>(part.size())) out_of_step.store(true);
}

Coro<void> read_counter(TeamMember, const CopyCounter&) { co_return; }

Coro<void> count_part(TeamMember, std::atomic<int>& ran) {
    ran.fetch_add(1);
    co_return;
}

Coro<void> wait_at_barrier(TeamMember part, [[maybe_unused]] DropToken token) {
    co_await part.sync();
}

// Holds its member, so the team stays whole while it waits.
Coro<void> wait_forever(TeamMember, [[maybe_unused]] DropToken token) {
    co_await never<void>();
}

// Holds its member until it throws, inside its task's poll: the team breaks only
// once the task is sure to report the exception.
Coro<void> throw_now(TeamMember part) {
    throw std::runtime_error("part " + std::to_string(part.rank()) + " failed");
    co_return;
}

// Ranks below `throwers` throw at once; the rest wait at the barrier for them, and
// fail with TeamBroken or are cancelled.
Coro<void> some_parts_throw(TeamMember part, std::atomic<int>& dropped, int throwers) {
    if (static_cast<int>(part.rank()) < throwers) return throw_now(std::move(part));
    return wait_at_barrier(std::move(part), DropToken{dropped});
}

// Every part waits for ever: two at the barrier, one on never().
Coro<void> never_finishes(TeamMember part, std::atomic<int>& dropped) {
    if (part.rank() == 0) return wait_forever(std::move(part), DropToken{dropped});
    return wait_at_barrier(std::move(part), DropToken{dropped});
}

// Throws while starting part 2: parts 0 and 1 are already spawned.
Coro<void> fails_to_start(TeamMember part, std::atomic<int>& dropped) {
    if (part.rank() == 2) throw std::runtime_error("could not start part 2");
    return wait_at_barrier(std::move(part), DropToken{dropped});
}

// Part 0 throws in the second round, while the others are in it: they fail with
// TeamBroken, possibly before part 0's exception reaches the drain.
Coro<void> throws_in_round_two(TeamMember part) {
    co_await part.sync();
    if (part.rank() == 0) throw std::runtime_error("part 0 failed");
    co_await part.sync();
}

struct CollectiveResults {
    std::array<std::size_t, kMaxParts> sum{};
    std::array<std::size_t, kMaxParts> prefix{};
    std::array<std::string, kMaxParts> broadcast{};
    std::array<bool, kMaxParts>        any{};
};

Coro<void> run_collectives(TeamMember part, CollectiveResults& out) {
    const std::size_t r = part.rank();
    out.sum[r]       = co_await part.all_reduce(r + 1);
    out.prefix[r]    = co_await part.exclusive_scan(r + 1, std::size_t{0});
    out.broadcast[r] = co_await part.broadcast(std::string(r == 2 ? "from 2" : "other"), 2);
    out.any[r]       = co_await part.any(r == part.size() - 1);
}

}  // namespace

static_assert(Cancellable<decltype(parallel(1, count_part, std::ref(std::declval<std::atomic<int>&>())))>);

// ---------------------------------------------------------------------------
// On each executor
// ---------------------------------------------------------------------------

template<typename Traits>
class ParallelTest : public testing::Test {
protected:
    Traits traits;
};
TYPED_TEST_SUITE(ParallelTest, AllExecutors);

TYPED_TEST(ParallelTest, EveryIndexRunsOnce) {
    constexpr std::size_t kParts = 5;
    Seen seen{};
    std::atomic<bool> bad_count = false;
    this->traits.rt.block_on([](Seen& seen, std::atomic<bool>& bad_count) -> Coro<void> {
        co_await parallel(kParts, record_index, std::ref(seen), std::ref(bad_count), kParts);
    }(seen, bad_count));
    EXPECT_FALSE(bad_count);
    for (std::size_t i = 0; i < kMaxParts; ++i)
        EXPECT_EQ(seen[i], i < kParts ? 1 : 0) << "index " << i;
}

TYPED_TEST(ParallelTest, OnePartPerWorker) {
    std::atomic<int> ran = 0;
    std::size_t workers = 0;
    this->traits.rt.block_on([](std::atomic<int>& ran, std::size_t& workers) -> Coro<void> {
        workers = worker_count();
        co_await parallel(workers, count_part, std::ref(ran));
    }(ran, workers));
    EXPECT_EQ(ran, static_cast<int>(workers));
}

TYPED_TEST(ParallelTest, SyncSeparatesPhases) {
    std::atomic<int> phase1 = 0, phase2 = 0;
    std::atomic<bool> out_of_step = false;
    this->traits.rt.block_on([](std::atomic<int>& phase1, std::atomic<int>& phase2,
                                std::atomic<bool>& out_of_step) -> Coro<void> {
        co_await parallel(4, two_phases, std::ref(phase1), std::ref(phase2),
                          std::ref(out_of_step));
    }(phase1, phase2, out_of_step));
    EXPECT_FALSE(out_of_step);
    EXPECT_EQ(phase2, 4);
}

TYPED_TEST(ParallelTest, ArgsCopiedOnceNotPerPart) {
    std::atomic<int> copies = 0;
    this->traits.rt.block_on([](std::atomic<int>& copies) -> Coro<void> {
        CopyCounter counter(copies);
        co_await parallel(4, read_counter, counter);  // one copy, into the call
    }(copies));
    EXPECT_EQ(copies, 1);
}

TYPED_TEST(ParallelTest, ThrowingPartCancelsTheOthers) {
    std::string what;
    int dropped_at_catch = -1;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](std::string& what, int& dropped_at_catch,
                                std::atomic<int>& dropped) -> Coro<void> {
        try {
            co_await parallel(4, some_parts_throw, std::ref(dropped), 1);
        } catch (const std::runtime_error& e) {
            what = e.what();
            dropped_at_catch = dropped.load();
        }
    }(what, dropped_at_catch, dropped));
    EXPECT_EQ(what, "part 0 failed");
    // The three parts at the barrier had finished before the rethrow.
    EXPECT_EQ(dropped_at_catch, 3);
}

TYPED_TEST(ParallelTest, TwoThrowingPartsRethrowOne) {
    int caught = 0;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](int& caught, std::atomic<int>& dropped) -> Coro<void> {
        try {
            co_await parallel(4, some_parts_throw, std::ref(dropped), 2);
        } catch (const std::runtime_error&) {
            ++caught;
        }
    }(caught, dropped));
    EXPECT_EQ(caught, 1);
    EXPECT_EQ(dropped, 2);
}

// A part's exception is reported, never the TeamBroken it causes in the others.
// Repeated: on a multi-threaded executor which reaches the drain first varies.
TYPED_TEST(ParallelTest, ThrowingPartIsReportedNotTeamBroken) {
    for (int i = 0; i < 50; ++i) {
        std::string what;
        this->traits.rt.block_on([](std::string& what) -> Coro<void> {
            try {
                co_await parallel(4, throws_in_round_two);
            } catch (const TeamBroken&) {
                what = "TeamBroken";
            } catch (const std::runtime_error& e) {
                what = e.what();
            }
        }(what));
        ASSERT_EQ(what, "part 0 failed") << "iteration " << i;
    }
}

TYPED_TEST(ParallelTest, CollectivesThroughTeamMember) {
    constexpr std::size_t kParts = 4;
    CollectiveResults out;
    this->traits.rt.block_on([](CollectiveResults& out) -> Coro<void> {
        co_await parallel(kParts, run_collectives, std::ref(out));
    }(out));
    std::size_t prefix = 0;
    for (std::size_t r = 0; r < kParts; ++r) {
        EXPECT_EQ(out.sum[r], 10u) << "rank " << r;
        EXPECT_EQ(out.prefix[r], prefix) << "rank " << r;
        EXPECT_EQ(out.broadcast[r], "from 2") << "rank " << r;
        EXPECT_TRUE(out.any[r]) << "rank " << r;
        prefix += r + 1;
    }
}

TYPED_TEST(ParallelTest, ExceptionStartingAPartCancelsTheStarted) {
    std::string what;
    int dropped_at_catch = -1;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](std::string& what, int& dropped_at_catch,
                                std::atomic<int>& dropped) -> Coro<void> {
        try {
            co_await parallel(4, fails_to_start, std::ref(dropped));
        } catch (const std::runtime_error& e) {
            what = e.what();
            dropped_at_catch = dropped.load();
        }
    }(what, dropped_at_catch, dropped));
    EXPECT_EQ(what, "could not start part 2");
    EXPECT_EQ(dropped_at_catch, 2);  // parts 0 and 1
}

TYPED_TEST(ParallelTest, CancelledCallerWaitsForEveryPart) {
    std::size_t winner = 0;
    int dropped_after_select = -1;
    std::atomic<int> dropped = 0;
    this->traits.rt.block_on([](std::size_t& winner, int& dropped_after_select,
                                std::atomic<int>& dropped) -> Coro<void> {
        auto sel = co_await select(parallel(3, never_finishes, std::ref(dropped)),
                                   ReadyVoidFuture{});
        winner = sel.index();
        dropped_after_select = dropped.load();
    }(winner, dropped_after_select, dropped));
    EXPECT_EQ(winner, 1u);
    // select() returned only once every part had finished.
    EXPECT_EQ(dropped_after_select, 3);
}

TYPED_TEST(ParallelTest, CancelledBeforeFirstPollSpawnsNothing) {
    std::size_t winner = 1;
    std::atomic<int> ran = 0;
    this->traits.rt.block_on([](std::size_t& winner, std::atomic<int>& ran) -> Coro<void> {
        auto sel = co_await select(ReadyVoidFuture{}, parallel(3, count_part, std::ref(ran)));
        winner = sel.index();
    }(winner, ran));
    EXPECT_EQ(winner, 0u);
    EXPECT_EQ(ran, 0);
}

TYPED_TEST(ParallelTest, MovedBeforeFirstPollSpawnsOnlyWhenPolled) {
    std::atomic<int> ran = 0;
    int ran_before_await = -1;
    this->traits.rt.block_on([](std::atomic<int>& ran, int& ran_before_await) -> Coro<void> {
        auto first = parallel(3, count_part, std::ref(ran));
        auto moved = std::move(first);
        ran_before_await = ran.load();
        co_await std::move(moved);
    }(ran, ran_before_await));
    EXPECT_EQ(ran_before_await, 0);
    EXPECT_EQ(ran, 3);
}

// ---------------------------------------------------------------------------
// WorkStealingExecutor
// ---------------------------------------------------------------------------

#ifndef CORO_PICO

namespace {

// Holds its worker, with no co_await, until every part has started or the deadline
// passes. Passes only if every part runs on a worker of its own at once.
Coro<void> hold_until_all_started(TeamMember part, std::atomic<int>& started,
                                  std::atomic<bool>& gave_up) {
    started.fetch_add(1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (started.load() < static_cast<int>(part.size())) {
        if (std::chrono::steady_clock::now() >= deadline) {
            gave_up.store(true);
            break;
        }
        std::this_thread::yield();
    }
    co_return;
}

}  // namespace

TEST(ParallelWorkStealingTest, PartsRunOnEveryWorkerAtOnce) {
    Runtime rt(std::size_t{4});
    std::atomic<int> started = 0;
    std::atomic<bool> gave_up = false;
    rt.block_on([](std::atomic<int>& started, std::atomic<bool>& gave_up) -> Coro<void> {
        co_await parallel(worker_count(), hold_until_all_started, std::ref(started),
                          std::ref(gave_up));
    }(started, gave_up));
    EXPECT_EQ(started, 4);
    EXPECT_FALSE(gave_up);
}

#endif  // !CORO_PICO
