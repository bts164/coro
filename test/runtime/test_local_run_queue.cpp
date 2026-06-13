#include <coro/detail/local_run_queue.h>
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace coro::detail;
using Task = std::shared_ptr<int>;

static Task make_task(int v) { return std::make_shared<int>(v); }

// Overflow sink that collects spilled tasks into a vector.
struct VecOverflow {
    std::vector<Task> tasks;
    void push(Task t) { tasks.push_back(std::move(t)); }
    void push_batch(Task* ts, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            tasks.push_back(std::move(ts[i]));
    }
};

// Drain ring + LIFO so the Local destructor assertion passes.
static void drain(Local<Task>& q) {
    while (q.pop()) {}
    q.pop_lifo();
}

static std::vector<int> drain_values(Local<Task>& q) {
    std::vector<int> out;
    while (auto t = q.pop()) out.push_back(*t);
    return out;
}

// ── empty behaviour ───────────────────────────────────────────────────────────

TEST(LocalRunQueue, PopEmptyReturnsFalsy) {
    auto [steal, local] = make_local_run_queue<Task>();
    EXPECT_FALSE(local.pop());
}

TEST(LocalRunQueue, PopLifoEmptyReturnsFalsy) {
    auto [steal, local] = make_local_run_queue<Task>();
    EXPECT_FALSE(local.pop_lifo());
}

TEST(LocalRunQueue, StealIntoEmptyReturnsFalsy) {
    auto [steal_src, src] = make_local_run_queue<Task>();
    auto [steal_dst, dst] = make_local_run_queue<Task>();
    EXPECT_FALSE(steal_src.steal_into(dst));
}

// ── ring: push / pop ──────────────────────────────────────────────────────────

TEST(LocalRunQueue, PushPopFifo) {
    auto [steal, local] = make_local_run_queue<Task>();
    local.push_back(make_task(1));
    local.push_back(make_task(2));
    local.push_back(make_task(3));

    EXPECT_EQ(*local.pop(), 1);
    EXPECT_EQ(*local.pop(), 2);
    EXPECT_EQ(*local.pop(), 3);
    EXPECT_FALSE(local.pop());
}

TEST(LocalRunQueue, PopReturnsFalsyWhenEmpty) {
    auto [steal, local] = make_local_run_queue<Task>();
    local.push_back(make_task(1));
    local.pop();
    EXPECT_FALSE(local.pop());
}

// ── capacity queries ──────────────────────────────────────────────────────────

TEST(LocalRunQueue, LenTracksRingAndLifo) {
    auto [steal, local] = make_local_run_queue<Task>();
    EXPECT_EQ(local.len(), 0u);
    EXPECT_FALSE(local.has_tasks());

    local.push_back(make_task(1));
    EXPECT_EQ(local.len(), 1u);
    EXPECT_TRUE(local.has_tasks());

    local.push_lifo(make_task(2));
    EXPECT_EQ(local.len(), 2u);

    local.pop();
    EXPECT_EQ(local.len(), 1u);

    local.pop_lifo();
    EXPECT_EQ(local.len(), 0u);
}

TEST(LocalRunQueue, RemainingSlots) {
    auto [steal, local] = make_local_run_queue<Task>();
    EXPECT_EQ(local.remaining_slots(), LOCAL_QUEUE_CAPACITY);
    local.push_back(make_task(1));
    EXPECT_EQ(local.remaining_slots(), LOCAL_QUEUE_CAPACITY - 1);
    local.pop();
    EXPECT_EQ(local.remaining_slots(), LOCAL_QUEUE_CAPACITY);
}

// ── LIFO slot ─────────────────────────────────────────────────────────────────

TEST(LocalRunQueue, PushLifoPopLifo) {
    auto [steal, local] = make_local_run_queue<Task>();
    auto displaced = local.push_lifo(make_task(42));
    EXPECT_FALSE(displaced);           // slot was empty
    auto t = local.pop_lifo();
    ASSERT_TRUE(t);
    EXPECT_EQ(*t, 42);
    EXPECT_FALSE(local.pop_lifo());    // now empty again
}

TEST(LocalRunQueue, PushLifoDisplacesExisting) {
    auto [steal, local] = make_local_run_queue<Task>();
    local.push_lifo(make_task(1));
    auto displaced = local.push_lifo(make_task(2));
    ASSERT_TRUE(displaced);
    EXPECT_EQ(*displaced, 1);
    auto t = local.pop_lifo();
    ASSERT_TRUE(t);
    EXPECT_EQ(*t, 2);
}

// ── push_or_overflow ──────────────────────────────────────────────────────────

TEST(LocalRunQueue, PushOrOverflowNoSpill) {
    auto [steal, local] = make_local_run_queue<Task>();
    VecOverflow overflow;
    local.push_or_overflow(make_task(7), overflow);
    EXPECT_TRUE(overflow.tasks.empty());
    EXPECT_EQ(local.len(), 1u);
    drain(local);
}

TEST(LocalRunQueue, PushOrOverflowSpillsHalfPlusOneWhenFull) {
    auto [steal, local] = make_local_run_queue<Task>();

    for (std::size_t i = 0; i < LOCAL_QUEUE_CAPACITY; ++i)
        local.push_back(make_task(static_cast<int>(i)));

    VecOverflow overflow;
    local.push_or_overflow(make_task(999), overflow);

    // Second half of ring + the new task spill; first half stays in the ring.
    EXPECT_EQ(overflow.tasks.size(), LOCAL_QUEUE_CAPACITY / 2 + 1);
    EXPECT_EQ(local.len(), LOCAL_QUEUE_CAPACITY / 2);

    drain(local);
}

// ── steal_into ────────────────────────────────────────────────────────────────

TEST(LocalRunQueue, StealIntoBasic) {
    auto [steal_src, src] = make_local_run_queue<Task>();
    auto [steal_dst, dst] = make_local_run_queue<Task>();

    for (int i = 1; i <= 8; ++i)
        src.push_back(make_task(i));

    auto t = steal_src.steal_into(dst);
    ASSERT_TRUE(t);

    // Victim lost roughly half; thief received the rest.
    const std::size_t stolen = dst.len() + 1;
    EXPECT_GE(stolen, 4u);
    EXPECT_LE(stolen, 5u);
    EXPECT_EQ(src.len() + stolen, 8u);

    drain(src);
    drain(dst);
}

TEST(LocalRunQueue, StealIntoReturnsOneToRunImmediately) {
    auto [steal_src, src] = make_local_run_queue<Task>();
    auto [steal_dst, dst] = make_local_run_queue<Task>();

    for (int i = 1; i <= 4; ++i)
        src.push_back(make_task(i));

    auto t = steal_src.steal_into(dst);
    EXPECT_TRUE(t);   // caller gets one task to run right away

    drain(src);
    drain(dst);
}

TEST(LocalRunQueue, StealIntoFallsBackToLifoSlotWhenRingEmpty) {
    auto [steal_src, src] = make_local_run_queue<Task>();
    auto [steal_dst, dst] = make_local_run_queue<Task>();

    src.push_lifo(make_task(55));

    auto t = steal_src.steal_into(dst);
    ASSERT_TRUE(t);
    EXPECT_EQ(*t, 55);
    EXPECT_EQ(dst.len(), 0u);  // lifo path places nothing into dst
}

TEST(LocalRunQueue, StealIntoDeclinesDstMoreThanHalfFull) {
    auto [steal_src, src] = make_local_run_queue<Task>();
    auto [steal_dst, dst] = make_local_run_queue<Task>();

    for (int i = 0; i < 4; ++i)
        src.push_back(make_task(i));

    // Fill dst past the half-capacity threshold.
    for (std::size_t i = 0; i < LOCAL_QUEUE_CAPACITY / 2 + 1; ++i)
        dst.push_back(make_task(0));

    EXPECT_FALSE(steal_src.steal_into(dst));  // dst too full — steal declined

    drain(src);
    drain(dst);
}

// ── threaded smoke test ───────────────────────────────────────────────────────

TEST(LocalRunQueue, ConcurrentPushAndSteal) {
    auto [steal_src, src] = make_local_run_queue<Task>();
    auto [steal_dst, dst] = make_local_run_queue<Task>();

    constexpr int N = 1000;
    std::atomic<int> consumed{0};

    std::thread producer([&] {
        VecOverflow overflow;
        for (int i = 0; i < N; ++i)
            src.push_or_overflow(make_task(i), overflow);
        consumed.fetch_add(static_cast<int>(overflow.tasks.size()),
                           std::memory_order_relaxed);
    });

    std::thread thief([&] {
        int count = 0;
        for (int attempt = 0; attempt < N * 4; ++attempt) {
            if (auto t = steal_src.steal_into(dst)) {
                ++count;
                while (dst.pop()) ++count;
            }
        }
        consumed.fetch_add(count, std::memory_order_relaxed);
    });

    producer.join();
    thief.join();

    while (src.pop())  consumed.fetch_add(1, std::memory_order_relaxed);
    while (dst.pop())  consumed.fetch_add(1, std::memory_order_relaxed);
    if (src.pop_lifo()) consumed.fetch_add(1, std::memory_order_relaxed);

    EXPECT_EQ(consumed.load(), N);
}
