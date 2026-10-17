#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <co_assert.h>
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#include <coro/sync/join.h>
#include <coro/task/join_handle.h>
#include <coro/task/join_set.h>
#include <coro/task/parallel.h>
#include <coro/task/spawn_builder.h>

using namespace coro;

//#define SKYNET_BENCHMARK_ITERATIONS 10

#ifdef SKYNET_BENCHMARK_ITERATIONS
// Runs make_future() to completion SKYNET_BENCHMARK_ITERATIONS times on rt and prints
// each iteration's wall time, so the first (cold heap) can be told from the rest.
template<typename MakeFuture>
void run_skynet(Runtime& rt, MakeFuture make_future) {
    using Clock = std::chrono::steady_clock;
    std::vector<double> ms;
    ms.reserve(SKYNET_BENCHMARK_ITERATIONS);
    for (size_t i = 0; i < SKYNET_BENCHMARK_ITERATIONS; ++i) {
        auto start = Clock::now();
        rt.block_on(make_future());
        ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }

    auto const* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::printf("%s.%s:\n", info->test_suite_name(), info->name());
    for (size_t i = 0; i < ms.size(); ++i)
        std::printf("  iter %2zu: %9.3f ms\n", i, ms[i]);
    auto sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double total = std::accumulate(ms.begin(), ms.end(), 0.0);
    std::printf("  min %.3f  median %.3f  max %.3f  mean %.3f  total %.3f ms\n",
                sorted.front(), sorted[sorted.size() / 2], sorted.back(),
                total / ms.size(), total);
}
#else
class SkynetLookup {
private:
    static SkynetLookup& initialize()
    {
        static SkynetLookup lookup;
        return lookup;
    }
    std::size_t skynet(size_t m, size_t r)
    {
        if (r == 1) {
            expected[m][r] = m;
            return m;
        }
        size_t sum = 0;
        for (size_t i = 0; i < 10; ++i)
            sum += skynet(m + i*(r/10), r/10);
        expected[m][r] = sum;
        return sum;
    }
    SkynetLookup() {
        skynet(0,1000000);
    }
    std::map<size_t, std::map<size_t, uint64_t>> expected;
public:
    static inline SkynetLookup const &instance = initialize();
    size_t operator[](size_t m, size_t r) const {
        return expected.at(m).at(r);
    }
};

// Runs make_future() once on rt and checks the tree's total.
template<typename MakeFuture>
void run_skynet(Runtime& rt, MakeFuture make_future) {
    size_t result = rt.block_on(make_future());
    EXPECT_EQ(result, (SkynetLookup::instance[0, 1000000]));
}
#endif

template<std::size_t... Is>
Coro<size_t> skynet_handles(size_t my_num, size_t remaining, std::index_sequence<Is...> seq) {
    if (remaining == 1) co_return my_num;

    static constexpr size_t N = sizeof...(Is);
    JoinHandle<size_t> handles[N] = {
        coro::spawn(
            skynet_handles(my_num + Is*(remaining/N), remaining/N, seq)
        )
        ...
    };
    auto results = co_await coro::join(std::move(handles[Is])...);
    size_t sum = (std::get<Is>(results) + ...);
    #ifndef SKYNET_BENCHMARK_ITERATIONS
    size_t exp = SkynetLookup::instance[my_num, remaining];
    CO_ASSERT_EQ(sum, exp)
        << sum << " != " << exp << " (my_num = " << my_num << ", remaining = " << remaining << ")";
    #endif
    co_return sum;
}

TEST(SkynetHandlesTest, SingleThreaded) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>);
    run_skynet(rt, [] { return skynet_handles(0, 1000000, std::make_index_sequence<10>{}); });
}

TEST(SkynetHandlesTest, Sharing) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>);
    run_skynet(rt, [] { return skynet_handles(0, 1000000, std::make_index_sequence<10>{}); });
}

TEST(SkynetHandlesTest, Stealing) {
    Runtime rt(std::in_place_type<WorkStealingExecutor>);
    run_skynet(rt, [] { return skynet_handles(0, 1000000, std::make_index_sequence<10>{}); });
}

Coro<size_t> skynet_join(size_t my_num, size_t remaining) {
    if (remaining == 1) co_return my_num;
    std::vector<JoinHandle<size_t>> handles;
    for (size_t i = 0; i < 10; ++i)
        handles.emplace_back(
            coro::spawn(skynet_join(my_num + i*(remaining/10), remaining/10))
        );
    size_t sum = 0;
    for (auto &h : handles) {
        sum += co_await h;
    }
    #ifndef SKYNET_BENCHMARK_ITERATIONS
    size_t exp = SkynetLookup::instance[my_num, remaining];
    CO_ASSERT_EQ(sum, exp)
        << sum << " != " << exp << " (my_num = " << my_num << ", remaining = " << remaining << ")";
    #endif
    co_return sum;
}

TEST(SkynetJoinTest, SingleThreaded) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>);
    run_skynet(rt, [] { return skynet_join(0, 1000000); });
}

TEST(SkynetJoinTest, Sharing) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>);
    run_skynet(rt, [] { return skynet_join(0, 1000000); });
}

TEST(SkynetJoinTest, Stealing) {
    Runtime rt(std::in_place_type<WorkStealingExecutor>);
    run_skynet(rt, [] { return skynet_join(0, 1000000); });
}

Coro<size_t> skynet_joinset(size_t my_num, size_t remaining) {
    if (remaining == 1) co_return my_num;
    JoinSet<size_t> js;
    for (size_t i = 0; i < 10; ++i)
        js.spawn(skynet_joinset(my_num + i*(remaining/10), remaining/10));
    size_t sum = 0;
    while (auto item = co_await next(js)) {
        sum += item.value();
    }
    #ifndef SKYNET_BENCHMARK_ITERATIONS
    size_t exp = SkynetLookup::instance[my_num, remaining];
    CO_ASSERT_EQ(sum, exp)
        << sum << " != " << exp << " (my_num = " << my_num << ", remaining = " << remaining << ")";
    #endif
    co_return sum;
}

// skynet_joinset as a hand-written future: the same JoinSet tree, but no coroutine frames.
// Comparing the two benchmarks shows what allocating and resuming the frames costs.
class SkynetJoinsetFuture
{
    enum class Stage {
        Start,
        Spawned,
        Done
    };
public:
    using OutputType = size_t;
    SkynetJoinsetFuture(size_t my_num, size_t remaining) :
        m_my_num(my_num), m_remaining(remaining)
    {}
    PollResult<OutputType> poll(detail::Context& ctx)
    {
        for (;;) {
            switch (m_stage) {
            case Stage::Start: {
                if (m_remaining == 1) {
                    m_stage = Stage::Done;
                    return m_my_num;
                }
                m_js.emplace();
                for (size_t i = 0; i < 10; ++i) {
                    m_js->spawn(SkynetJoinsetFuture(m_my_num + i*(m_remaining/10), m_remaining/10));
                }
                m_next.emplace(next(m_js.value()));
                m_stage = Stage::Spawned;
            } break;
            case Stage::Spawned: {
                auto result = m_next.value().poll(ctx);
                if (result.isPending()) {
                    return PollPending;
                }
                if (result.isError()) {
                    m_stage = Stage::Done;
                    return PollError(result.error());
                }
                auto item = std::move(result).value();
                if (item.has_value()) {
                    m_sum += item.value();
                    m_next.emplace(next(m_js.value()));
                    break;
                }
                m_stage = Stage::Done;
                #ifndef SKYNET_BENCHMARK_ITERATIONS
                size_t exp = SkynetLookup::instance[m_my_num, m_remaining];
                if (m_sum != exp) {
                    return PollError(std::make_exception_ptr(std::runtime_error(
                        std::to_string(m_sum) + " != " + std::to_string(exp) + " (my_num = "
                        + std::to_string(m_my_num) + ", remaining = " + std::to_string(m_remaining) + ")")));
                }
                #endif
                return m_sum;
            }
            case Stage::Done:
                return PollError(std::make_exception_ptr(std::runtime_error("poll() called after completion")));
            }
        }
    }
private:
    size_t m_my_num;
    size_t m_remaining;
    Stage m_stage = Stage::Start;
    size_t m_sum = 0;
    std::optional<JoinSet<size_t>> m_js;
    std::optional<NextFuture<JoinSet<size_t>>> m_next;
};

TEST(SkynetJoinSetTest, SingleThreaded) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>);
    run_skynet(rt, [] { return skynet_joinset(0, 1000000); });
}

TEST(SkynetJoinSetTest, Sharing) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>);
    run_skynet(rt, [] { return skynet_joinset(0, 1000000); });
}

TEST(SkynetJoinSetTest, Stealing) {
    Runtime rt(std::in_place_type<WorkStealingExecutor>);
    run_skynet(rt, [] { return skynet_joinset(0, 1000000); });
}

TEST(SkynetJoinSetFutureTest, SingleThreaded) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>);
    run_skynet(rt, [] { return SkynetJoinsetFuture(0, 1000000); });
}

TEST(SkynetJoinSetFutureTest, Sharing) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>);
    run_skynet(rt, [] { return SkynetJoinsetFuture(0, 1000000); });
}

TEST(SkynetJoinSetFutureTest, Stealing) {
    Runtime rt(std::in_place_type<WorkStealingExecutor>);
    run_skynet(rt, [] { return SkynetJoinsetFuture(0, 1000000); });
}

template<std::size_t... Is>
Coro<size_t> skynet_mixed1(size_t my_num, size_t remaining, std::index_sequence<Is...> seq) {
    if (remaining == 1) co_return my_num;
    auto results = co_await join(skynet_mixed1(my_num + Is*(remaining/10), remaining/10, seq)...);
    size_t sum = (std::get<Is>(results) + ...);
    #ifndef SKYNET_BENCHMARK_ITERATIONS
    size_t exp = SkynetLookup::instance[my_num, remaining];
    CO_ASSERT_EQ(sum, exp)
        << sum << " != " << exp << " (my_num = " << my_num << ", remaining = " << remaining << ")";
    #endif
    co_return sum;
}

Coro<size_t> skynet_mixed0(size_t my_num, size_t remaining) {
    if (remaining == 1) co_return my_num;
    JoinSet<size_t> js;
    for (size_t i = 0; i < 10; ++i)
        js.spawn(skynet_mixed1(my_num + i*(remaining/10), remaining/10, std::make_index_sequence<10>{}));
    size_t sum = 0;
    while (auto item = co_await next(js)) {
        sum += item.value();
    }
    #ifndef SKYNET_BENCHMARK_ITERATIONS
    size_t exp = SkynetLookup::instance[my_num, remaining];
    CO_ASSERT_EQ(sum, exp)
        << sum << " != " << exp << " (my_num = " << my_num << ", remaining = " << remaining << ")";
    #endif
    co_return sum;
}

TEST(SkynetMixedTest, SingleThreaded) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>);
    run_skynet(rt, [] { return skynet_mixed0(0, 1000000); });
}

TEST(SkynetMixedTest, Sharing) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>);
    run_skynet(rt, [] { return skynet_mixed0(0, 1000000); });
}

TEST(SkynetMixedTest, Stealing) {
    Runtime rt(std::in_place_type<WorkStealingExecutor>);
    run_skynet(rt, [] { return skynet_mixed0(0, 1000000); });
}
// parallel() at every level: each node is one part of its parent's parallel() call and
// writes its sum into the parent's array. Every node carries a team, a JoinSet and a
// WakeBatch, so on WorkStealingExecutor every spawn goes through the injection queue.
Coro<void> skynet_parallel_node(TeamMember part, size_t parent_num, size_t remaining,
                                std::array<size_t, 10>& parent_sums) {
    const size_t my_num = parent_num + part.rank()*remaining;
    if (remaining == 1) {
        parent_sums[part.rank()] = my_num;
        co_return;
    }
    std::array<size_t, 10> sums{};
    co_await parallel(10, skynet_parallel_node, my_num, remaining/10, std::ref(sums));
    size_t sum = std::accumulate(sums.begin(), sums.end(), size_t{0});
    #ifndef SKYNET_BENCHMARK_ITERATIONS
    size_t exp = SkynetLookup::instance[my_num, remaining];
    CO_ASSERT_EQ(sum, exp)
        << sum << " != " << exp << " (my_num = " << my_num << ", remaining = " << remaining << ")";
    #endif
    parent_sums[part.rank()] = sum;
}

Coro<size_t> skynet_parallel(size_t remaining) {
    std::array<size_t, 10> sums{};
    co_await parallel(10, skynet_parallel_node, size_t{0}, remaining/10, std::ref(sums));
    co_return std::accumulate(sums.begin(), sums.end(), size_t{0});
}

TEST(SkynetParallelTest, SingleThreaded) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>);
    run_skynet(rt, [] { return skynet_parallel(1000000); });
}

TEST(SkynetParallelTest, Sharing) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>);
    run_skynet(rt, [] { return skynet_parallel(1000000); });
}

TEST(SkynetParallelTest, Stealing) {
    Runtime rt(std::in_place_type<WorkStealingExecutor>);
    run_skynet(rt, [] { return skynet_parallel(1000000); });
}

// parallel() at the first level only, then join() below: SkynetMixedTest with parallel()
// in place of the JoinSet.
Coro<void> skynet_parallel_mixed_part(TeamMember part, size_t remaining,
                                      std::array<size_t, 10>& sums) {
    sums[part.rank()] = co_await skynet_mixed1(part.rank()*remaining, remaining,
                                                std::make_index_sequence<10>{});
}

Coro<size_t> skynet_parallel_mixed(size_t remaining) {
    std::array<size_t, 10> sums{};
    co_await parallel(10, skynet_parallel_mixed_part, remaining/10, std::ref(sums));
    co_return std::accumulate(sums.begin(), sums.end(), size_t{0});
}

TEST(SkynetParallelMixedTest, SingleThreaded) {
    Runtime rt(std::in_place_type<CurrentThreadExecutor>);
    run_skynet(rt, [] { return skynet_parallel_mixed(1000000); });
}

TEST(SkynetParallelMixedTest, Sharing) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>);
    run_skynet(rt, [] { return skynet_parallel_mixed(1000000); });
}

TEST(SkynetParallelMixedTest, Stealing) {
    Runtime rt(std::in_place_type<WorkStealingExecutor>);
    run_skynet(rt, [] { return skynet_parallel_mixed(1000000); });
}
