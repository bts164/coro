// Tests for coro::detail::TimerQueue — ordering, lazy cancellation, and the
// waiter record that tells insert() when to unpark. No executor or driver involved.
// See doc/design/io_driver.md, "Timers".

#include <gtest/gtest.h>
#include <coro/detail/timer_queue.h>

#include <chrono>
#include <memory>
#include <mutex>
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
    Rc<TimerSlot>                 slot = make_rc<TimerSlot>();

    Timer(int id, std::vector<int>* log) : waker(std::make_shared<LoggingWaker>(id, log)) {
        std::lock_guard lock(slot->mutex);
        slot->waker = waker;
    }
};

} // namespace

TEST(TimerQueue, FiresInDeadlineOrder) {
    TimerQueue q;
    std::vector<int> log;
    const Instant past = Clock::now() - 1s;
    Timer t1(1, &log), t2(2, &log), t3(3, &log);
    q.insert(past + 3ms, t3.slot);
    q.insert(past + 1ms, t1.slot);
    q.insert(past + 2ms, t2.slot);

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
        q.insert(past, timers.back()->slot);
    }

    EXPECT_EQ(q.fire_expired(), 8u);
    EXPECT_EQ(log, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
}

TEST(TimerQueue, FutureDeadlineIsNotFired) {
    TimerQueue q;
    std::vector<int> log;
    Timer early(1, &log), late(2, &log);
    q.insert(Clock::now() - 1ms, early.slot);
    q.insert(Clock::now() + 1h, late.slot);

    EXPECT_EQ(q.fire_expired(), 1u);
    EXPECT_EQ(log, (std::vector<int>{1}));
    EXPECT_EQ(q.size(), 1u);
}

TEST(TimerQueue, EmptiedSlotIsPoppedWithoutWake) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.insert(Clock::now() - 1ms, t.slot);
    {
        std::lock_guard lock(t.slot->mutex);
        t.slot->waker.reset();   // what SleepFuture::release() does
    }

    EXPECT_EQ(q.fire_expired(), 0u);
    EXPECT_TRUE(log.empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(TimerQueue, ExpiredWakerIsPoppedWithoutWake) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.insert(Clock::now() - 1ms, t.slot);
    t.waker.reset();   // the task is gone; the slot's weak reference dangles

    EXPECT_EQ(q.fire_expired(), 0u);
    EXPECT_TRUE(log.empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(TimerQueue, FiringTakesTheWakerOutOfTheSlot) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.insert(Clock::now() - 1ms, t.slot);
    q.fire_expired();

    std::lock_guard lock(t.slot->mutex);
    EXPECT_TRUE(t.slot->waker.expired());
}

TEST(TimerQueue, InsertWithoutWaiterNeverAsksForUnpark) {
    TimerQueue q;
    std::vector<int> log;
    Timer t1(1, &log), t2(2, &log);
    EXPECT_FALSE(q.insert(Clock::now() + 1h, t1.slot));
    EXPECT_FALSE(q.insert(Clock::now() + 1ms, t2.slot));
}

TEST(TimerQueue, InsertEarlierThanFrontUnparksWaiter) {
    TimerQueue q;
    std::vector<int> log;
    Timer late(1, &log), later(2, &log), earlier(3, &log);
    q.insert(Clock::now() + 1h, late.slot);

    ASSERT_TRUE(q.begin_wait(std::nullopt).has_value());
    EXPECT_FALSE(q.insert(Clock::now() + 2h, later.slot));
    EXPECT_TRUE(q.insert(Clock::now() + 1min, earlier.slot));
    q.end_wait();
}

TEST(TimerQueue, InsertIntoEmptyQueueUnparksUnboundedWaiter) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    EXPECT_FALSE(q.begin_wait(std::nullopt).has_value());
    EXPECT_TRUE(q.insert(Clock::now() + 1h, t.slot));
    q.end_wait();
}

TEST(TimerQueue, EndWaitClearsWaiter) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.begin_wait(std::nullopt);
    q.end_wait();
    EXPECT_FALSE(q.insert(Clock::now() + 1h, t.slot));
}

TEST(TimerQueue, EndWaitAndFireExpiredClearsWaiterAndFires) {
    TimerQueue q;
    std::vector<int> log;
    Timer due(1, &log), later(2, &log), earlier(3, &log);
    q.insert(Clock::now() + 2h, later.slot);

    ASSERT_TRUE(q.begin_wait(std::nullopt).has_value());  // waiting ~2h: recorded
    EXPECT_TRUE(q.insert(Clock::now() - 1ms, due.slot));  // e.g. during dispatch
    EXPECT_EQ(q.end_wait_and_fire_expired(), 1u);
    EXPECT_EQ(log, (std::vector<int>{1}));
    EXPECT_FALSE(q.insert(Clock::now() + 1min, earlier.slot));  // no waiter left
}

TEST(TimerQueue, EndWaitAndFireExpiredClearsWaiterOnEmptyQueue) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.begin_wait(std::nullopt);
    EXPECT_EQ(q.end_wait_and_fire_expired(), 0u);
    EXPECT_FALSE(q.insert(Clock::now() + 1h, t.slot));
}

TEST(TimerQueue, ZeroWaitDoesNotRecordWaiter) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    EXPECT_EQ(q.begin_wait(0ns), 0ns);
    EXPECT_FALSE(q.insert(Clock::now() + 1h, t.slot));
}

TEST(TimerQueue, BeginWaitIsBoundedByEarliestDeadline) {
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.insert(Clock::now() + 50ms, t.slot);

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
    q.insert(Clock::now() - 1ms, t1.slot);

    EXPECT_EQ(q.begin_wait(std::nullopt), 0ns);
    // Zero wait: no waiter recorded.
    EXPECT_FALSE(q.insert(Clock::now() - 2ms, t2.slot));
    q.end_wait();
}

TEST(TimerQueue, CancelledEntryStillBoundsTheWait) {
    // Lazy cancellation: an emptied slot keeps its heap entry until popped, so the
    // waiter wakes for it once (and fires nothing). Documented cost, not a bug.
    TimerQueue q;
    std::vector<int> log;
    Timer t(1, &log);
    q.insert(Clock::now() + 10ms, t.slot);
    {
        std::lock_guard lock(t.slot->mutex);
        t.slot->waker.reset();
    }
    auto wait = q.begin_wait(std::nullopt);
    q.end_wait();
    ASSERT_TRUE(wait.has_value());
    EXPECT_LE(*wait, 10ms);
    EXPECT_EQ(q.size(), 1u);
}
