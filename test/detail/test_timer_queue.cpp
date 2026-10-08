// Tests for coro::detail::TimerQueue — ordering, cancellation by id (lazy, with a
// sweep), slot reuse, and the waiter record that tells insert() when to unpark. No
// executor or driver involved.
// See doc/design/io_driver.md, "Timers".

#include <gtest/gtest.h>
#include <coro/detail/timer_queue.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using namespace coro;
using namespace coro::detail;
using namespace std::chrono_literals;

namespace {

// Appends its id to a shared log on every wake.
class LoggingWaker : public Waker, public std::enable_shared_from_this<LoggingWaker> {
public:
    LoggingWaker(int id, std::vector<int>* log) : m_id(id), m_log(log) {}
    void wake() override { m_log->push_back(m_id); }
    Rc<Waker> clone() override { return shared_from_this(); }
private:
    int               m_id;
    std::vector<int>* m_log;
};

struct Timer {
    std::shared_ptr<LoggingWaker> waker;

    Timer(int id, std::vector<int>* log) : waker(std::make_shared<LoggingWaker>(id, log)) {}
};

// The slot index half of an id.
std::uint32_t slot_of(TimerId id) { return static_cast<std::uint32_t>(id); }

} // namespace

TEST(TimerQueue, FiresInDeadlineOrder) {
    TimerQueue q;
    std::vector<int> log;
    const Instant past = Clock::now() - 1s;
    Timer t1(1, &log), t2(2, &log), t3(3, &log);
    q.insert(past + 3ms, t3.waker);
    q.insert(past + 1ms, t1.waker);
    q.insert(past + 2ms, t2.waker);

    EXPECT_EQ(q.fire_expired(), 3u);
    EXPECT_EQ(log, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(q.size(), 0u);
}

TEST(TimerQueue, EqualDeadlinesFireInInsertionOrder) {
    TimerQueue q;
    std::vector<int> log;
    const Instant past = Clock::now() - 1s;
    std::vector<std::unique_ptr<Timer>> timers;
    for (int i = 0; i < 8; ++i) {
        timers.push_back(std::make_unique<Timer>(i, &log));
        q.insert(past, timers.back()->waker);
    }

    EXPECT_EQ(q.fire_expired(), 8u);
    EXPECT_EQ(log, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
}

TEST(TimerQueue, FutureDeadlineIsNotFired) {
    TimerQueue q;
    std::vector<int> log;
    Timer early(1, &log), late(2, &log);
    q.insert(Clock::now() - 1ms, early.waker);
    q.insert(Clock::now() + 1h, late.waker);

    EXPECT_EQ(q.fire_expired(), 1u);
    EXPECT_EQ(log, (std::vector<int>{1}));
    EXPECT_EQ(q.size(), 1u);
}

TEST(TimerQueue, CancelledTimerIsPoppedWithoutWake) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    const TimerId id = q.insert(Clock::now() - 1ms, t.waker).id;
    q.cancel(id);
    EXPECT_EQ(q.size(), 1u);   // lazy: the entry stays until popped

    EXPECT_EQ(q.fire_expired(), 0u);
    EXPECT_TRUE(log.empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(TimerQueue, ExpiredWakerIsPoppedWithoutWake) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.insert(Clock::now() - 1ms, t.waker);
    t.waker.reset();   // the task is gone; the slot's weak reference dangles

    EXPECT_EQ(q.fire_expired(), 0u);
    EXPECT_TRUE(log.empty());
    EXPECT_EQ(q.size(), 0u);
}

// An id whose timer has fired, or was already cancelled, cancels nothing.
TEST(TimerQueue, StaleIdIsIgnored) {
    TimerQueue q;
    std::vector<int> log;
    Timer fired(1, &log), cancelled(2, &log);
    const TimerId fired_id = q.insert(Clock::now() - 1ms, fired.waker).id;
    EXPECT_EQ(q.fire_expired(), 1u);
    q.cancel(fired_id);

    const TimerId cancelled_id = q.insert(Clock::now() - 1ms, cancelled.waker).id;
    q.cancel(cancelled_id);
    q.cancel(cancelled_id);
    EXPECT_EQ(q.fire_expired(), 0u);

    // Neither stale cancel was counted: the queue is empty and still works.
    EXPECT_EQ(q.size(), 0u);
    Timer next(3, &log);
    q.insert(Clock::now() - 1ms, next.waker);
    EXPECT_EQ(q.fire_expired(), 1u);
    EXPECT_EQ(log, (std::vector<int>{1, 3}));
}

// An id that was never issued (default-constructed queue, arbitrary bits) is ignored.
TEST(TimerQueue, UnknownIdIsIgnored) {
    TimerQueue q;
    q.cancel(0);
    q.cancel(~TimerId{0});
    EXPECT_EQ(q.size(), 0u);
}

TEST(TimerQueue, SlotsAreReused) {
    TimerQueue q;
    std::vector<int> log;
    Timer t1(1, &log), t2(2, &log);
    const TimerId first = q.insert(Clock::now() - 1ms, t1.waker).id;
    EXPECT_EQ(q.fire_expired(), 1u);
    const TimerId second = q.insert(Clock::now() - 1ms, t2.waker).id;

    EXPECT_EQ(slot_of(first), slot_of(second));
    EXPECT_NE(first, second);   // the generation differs
}

// The stale id of a slot's previous timer must not cancel its next one.
TEST(TimerQueue, CancelAfterSlotReuseLeavesNewTimer) {
    TimerQueue q;
    std::vector<int> log;
    Timer t1(1, &log), t2(2, &log);
    const TimerId first = q.insert(Clock::now() - 1ms, t1.waker).id;
    EXPECT_EQ(q.fire_expired(), 1u);
    const TimerId second = q.insert(Clock::now() - 1ms, t2.waker).id;
    ASSERT_EQ(slot_of(first), slot_of(second));

    q.cancel(first);
    EXPECT_EQ(q.fire_expired(), 1u);
    EXPECT_EQ(log, (std::vector<int>{1, 2}));
}

// Cancelled entries are removed once they outnumber the live ones, and the timers
// left behind still fire in order.
TEST(TimerQueue, SweepRemovesCancelledEntries) {
    constexpr int kTimers = 200;
    TimerQueue q;
    std::vector<int> log;
    const Instant past = Clock::now() - 1s;
    std::vector<std::unique_ptr<Timer>> timers;
    std::vector<TimerId> ids;
    for (int i = 0; i < kTimers; ++i) {
        timers.push_back(std::make_unique<Timer>(i, &log));
        ids.push_back(q.insert(past + std::chrono::microseconds(i), timers.back()->waker).id);
    }

    // Cancel the even ones: 100 cancelled against 100 live is not yet a majority.
    for (int i = 0; i < kTimers; i += 2) q.cancel(ids[i]);
    EXPECT_EQ(q.size(), 200u);

    // One more tips it: 101 cancelled, 99 live.
    q.cancel(ids[1]);
    EXPECT_EQ(q.size(), 99u);

    // The swept slots are reused, and their old ids are stale.
    Timer extra(1000, &log);
    q.insert(past + 1ms, extra.waker);
    q.cancel(ids[0]);
    EXPECT_EQ(q.size(), 100u);

    std::vector<int> expected;
    for (int i = 3; i < kTimers; i += 2) expected.push_back(i);
    expected.push_back(1000);
    EXPECT_EQ(q.fire_expired(), 100u);
    EXPECT_EQ(log, expected);
    EXPECT_EQ(q.size(), 0u);
}

// A small queue doesn't rebuild its heap on every cancel.
TEST(TimerQueue, NoSweepBelowMinimum) {
    const int count = static_cast<int>(TimerQueue::kSweepMinCancelled) - 1;
    TimerQueue q;
    std::vector<int> log;
    std::vector<std::unique_ptr<Timer>> timers;
    for (int i = 0; i < count; ++i) {
        timers.push_back(std::make_unique<Timer>(i, &log));
        q.cancel(q.insert(Clock::now() + 1h, timers.back()->waker).id);
    }
    EXPECT_EQ(q.size(), static_cast<std::size_t>(count));

    // The next cancel reaches the minimum with nothing live, and sweeps them all.
    Timer last(count, &log);
    q.cancel(q.insert(Clock::now() + 1h, last.waker).id);
    EXPECT_EQ(q.size(), 0u);
}

// More timers due at once than fire_expired() wakes per batch.
TEST(TimerQueue, FiresMoreThanOneBatch) {
    constexpr int kTimers = 100;
    TimerQueue q;
    std::vector<int> log;
    const Instant past = Clock::now() - 1s;
    std::vector<std::unique_ptr<Timer>> timers;
    std::vector<int> expected;
    for (int i = 0; i < kTimers; ++i) {
        timers.push_back(std::make_unique<Timer>(i, &log));
        q.insert(past, timers.back()->waker);
        expected.push_back(i);
    }

    EXPECT_EQ(q.fire_expired(), 100u);
    EXPECT_EQ(log, expected);
    EXPECT_EQ(q.size(), 0u);
}

TEST(TimerQueue, InsertWithoutWaiterNeverAsksForUnpark) {
    TimerQueue q;
    std::vector<int> log;
    Timer t1(1, &log), t2(2, &log);
    EXPECT_FALSE(q.insert(Clock::now() + 1h, t1.waker).unpark);
    EXPECT_FALSE(q.insert(Clock::now() + 1ms, t2.waker).unpark);
}

TEST(TimerQueue, InsertEarlierThanFrontUnparksWaiter) {
    TimerQueue q;
    std::vector<int> log;
    Timer late(1, &log), later(2, &log), earlier(3, &log);
    q.insert(Clock::now() + 1h, late.waker);

    ASSERT_TRUE(q.begin_wait(std::nullopt).has_value());
    EXPECT_FALSE(q.insert(Clock::now() + 2h, later.waker).unpark);
    EXPECT_TRUE(q.insert(Clock::now() + 1min, earlier.waker).unpark);
    q.end_wait();
}

TEST(TimerQueue, InsertIntoEmptyQueueUnparksUnboundedWaiter) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    EXPECT_FALSE(q.begin_wait(std::nullopt).has_value());
    EXPECT_TRUE(q.insert(Clock::now() + 1h, t.waker).unpark);
    q.end_wait();
}

TEST(TimerQueue, EndWaitClearsWaiter) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.begin_wait(std::nullopt);
    q.end_wait();
    EXPECT_FALSE(q.insert(Clock::now() + 1h, t.waker).unpark);
}

TEST(TimerQueue, EndWaitAndFireExpiredClearsWaiterAndFires) {
    TimerQueue q;
    std::vector<int> log;
    Timer due(1, &log), later(2, &log), earlier(3, &log);
    q.insert(Clock::now() + 2h, later.waker);

    ASSERT_TRUE(q.begin_wait(std::nullopt).has_value());  // waiting ~2h: recorded
    EXPECT_TRUE(q.insert(Clock::now() - 1ms, due.waker).unpark);  // e.g. during dispatch
    EXPECT_EQ(q.end_wait_and_fire_expired(), 1u);
    EXPECT_EQ(log, (std::vector<int>{1}));
    EXPECT_FALSE(q.insert(Clock::now() + 1min, earlier.waker).unpark);  // no waiter left
}

TEST(TimerQueue, EndWaitAndFireExpiredClearsWaiterOnEmptyQueue) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.begin_wait(std::nullopt);
    EXPECT_EQ(q.end_wait_and_fire_expired(), 0u);
    EXPECT_FALSE(q.insert(Clock::now() + 1h, t.waker).unpark);
}

TEST(TimerQueue, ZeroWaitDoesNotRecordWaiter) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    EXPECT_EQ(q.begin_wait(0ns), 0ns);
    EXPECT_FALSE(q.insert(Clock::now() + 1h, t.waker).unpark);
}

TEST(TimerQueue, BeginWaitIsBoundedByEarliestDeadline) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.insert(Clock::now() + 50ms, t.waker);

    auto unbounded = q.begin_wait(std::nullopt);
    q.end_wait();
    ASSERT_TRUE(unbounded.has_value());
    EXPECT_GT(*unbounded, 0ns);
    EXPECT_LE(*unbounded, 50ms);

    // A shorter max_wait wins over the deadline.
    EXPECT_EQ(q.begin_wait(1ms), 1ms);
    q.end_wait();

    // A longer one is cut down to it.
    auto bounded = q.begin_wait(1h);
    q.end_wait();
    ASSERT_TRUE(bounded.has_value());
    EXPECT_LE(*bounded, 50ms);
}

TEST(TimerQueue, BeginWaitWithPassedDeadlineIsZero) {
    TimerQueue q;
    std::vector<int> log;
    Timer t1(1, &log), t2(2, &log);
    q.insert(Clock::now() - 1ms, t1.waker);

    EXPECT_EQ(q.begin_wait(std::nullopt), 0ns);
    // Zero wait: no waiter recorded.
    EXPECT_FALSE(q.insert(Clock::now() - 2ms, t2.waker).unpark);
    q.end_wait();
}

TEST(TimerQueue, CancelledEntryStillBoundsTheWait) {
    // Lazy cancellation: a cancelled timer keeps its heap entry until popped or swept,
    // so the waiter wakes for it once (and fires nothing). Documented cost, not a bug.
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.cancel(q.insert(Clock::now() + 10ms, t.waker).id);

    auto wait = q.begin_wait(std::nullopt);
    q.end_wait();
    ASSERT_TRUE(wait.has_value());
    EXPECT_LE(*wait, 10ms);
    EXPECT_EQ(q.size(), 1u);
}

// A sweep can remove the entry a blocked waiter computed its timeout from. An insert
// later than that entry but earlier than the new front asks for an unpark it doesn't
// strictly need; one into the emptied heap must ask.
TEST(TimerQueue, InsertAfterSweepStillUnparksWaiter) {
    TimerQueue q;
    std::vector<int> log;
    std::vector<std::unique_ptr<Timer>> timers;
    std::vector<TimerId> ids;
    for (std::size_t i = 0; i < TimerQueue::kSweepMinCancelled; ++i) {
        timers.push_back(std::make_unique<Timer>(static_cast<int>(i), &log));
        ids.push_back(q.insert(Clock::now() + 1min, timers.back()->waker).id);
    }
    ASSERT_TRUE(q.begin_wait(std::nullopt).has_value());
    for (TimerId id : ids) q.cancel(id);
    ASSERT_EQ(q.size(), 0u);

    Timer t(1000, &log);
    EXPECT_TRUE(q.insert(Clock::now() + 1h, t.waker).unpark);
    q.end_wait();
}
