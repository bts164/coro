#include <atomic>
#include <optional>

#include <co_assert.h>
#include <coro/coro.h>
#include <coro/runtime/runtime.h>
#include <coro/runtime/single_threaded_executor.h>
#include <coro/runtime/work_sharing_executor.h>
#include <coro/runtime/work_stealing_executor.h>
#include <coro/sync/join.h>
#include <coro/task/join_handle.h>
#include <coro/task/join_set.h>
#include <coro/task/spawn_builder.h>

using namespace coro;

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

class ArgMarker
{
    ArgMarker() = default;
public:
    static inline std::atomic_size_t depth_counters[7];
    static inline std::atomic_size_t clone_depth_counters[7];
    static void reset_counters() {
        for (auto& c : depth_counters)
            c.store(0, std::memory_order_release);
        for (auto& c : clone_depth_counters)
            c.store(0, std::memory_order_release);
    }

    ArgMarker(std::atomic_size_t& id, size_t depth) :
        m_id(&id),
        m_depth(depth)
    {
        m_id->store(0, std::memory_order_release);
    }
    ArgMarker clone() const
    {
        ArgMarker a;
        a.m_id = m_id;
        a.m_clone_depth = m_depth;
        return a;
    }
    ArgMarker(const ArgMarker&) = delete;
    ArgMarker(ArgMarker&&other) :
        m_id(std::exchange(other.m_id, nullptr)),
        m_depth(std::exchange(other.m_depth, std::nullopt)),
        m_clone_depth(std::exchange(other.m_clone_depth, std::nullopt))
    {}
    ArgMarker& operator=(const ArgMarker&) = delete;
    ArgMarker& operator=(ArgMarker&&) = delete;
    ~ArgMarker()
    {
        if (m_id) {
            m_id->fetch_add(1);
        }
        if (m_depth) {
            depth_counters[m_depth.value()].fetch_add(1);
            //std::cout << "Set arg to true\n" << std::flush;
        }
        if (m_clone_depth) {
            clone_depth_counters[m_clone_depth.value()].fetch_add(1);
            //std::cout << "Set arg to true\n" << std::flush;
        }
    }
private:
    std::atomic_size_t *m_id = nullptr;
    std::optional<std::size_t> m_depth;
    std::optional<std::size_t> m_clone_depth;
};

template<std::size_t... Is>
Coro<size_t> skynet_handles(size_t my_num, size_t remaining, size_t depth, ArgMarker arg_lifetime_marker, std::index_sequence<Is...> seq) {
    size_t sum;
    {
        auto arg = arg_lifetime_marker.clone();
        if (remaining == 1) co_return my_num;

        static constexpr size_t N = sizeof...(Is);
        std::atomic_size_t arg_markers[N];
        JoinHandle<size_t> handles[N] = {
            coro::spawn(
                skynet_handles(my_num + Is*(remaining/N), remaining/N, depth - 1, ArgMarker(arg_markers[Is], depth - 1), std::make_index_sequence<N>{})
            ).submit()
            ...
        };
        auto results = co_await coro::join(std::move(handles[Is])...);
        for (size_t i = 0; i < N; ++i) {
            size_t n = arg_markers[i].load(std::memory_order_acquire);
            CO_ASSERT_EQ(n, 2)
                << "Argument lifetimes not as expected: " << n << " != 2 (" << my_num << ", " << remaining << ", " << i << ") not set after completion";
        }
        sum = ((std::get<Is>(results) + ...));
    }
    size_t exp = SkynetLookup::instance[my_num, remaining];
    CO_ASSERT_EQ(sum, exp)
        << sum << " != " << exp << " (my_num = " << my_num << ", remaining = " << remaining << ")";
    co_return sum;
}
TEST(SkynetHandlesTest, Single) {
    Runtime rt(std::in_place_type<SingleThreadedExecutor>);
    std::atomic_size_t arg_marker{0};
    rt.block_on(skynet_handles(0, 1000000, 6, ArgMarker(arg_marker, 6), std::make_index_sequence<10>{}));
    ASSERT_TRUE(arg_marker.load(std::memory_order_acquire)) << "Expected argument marker not set";
}

TEST(SkynetHandlesTest, Sharing) {
    Runtime rt(std::in_place_type<WorkSharingExecutor>);
    std::atomic_size_t arg_marker{0};
    rt.block_on(skynet_handles(0, 1000000, 6, ArgMarker(arg_marker, 6), std::make_index_sequence<10>{}));
    ASSERT_TRUE(arg_marker.load(std::memory_order_acquire)) << "Expected argument marker not set";
}

TEST(SkynetHandlesTest, Stealing) {
    Runtime rt(std::in_place_type<WorkStealingExecutor>);
    std::atomic_size_t arg_marker{0};
    rt.block_on(skynet_handles(0, 1000000, 6, ArgMarker(arg_marker, 6), std::make_index_sequence<10>{}));
    ASSERT_TRUE(arg_marker.load(std::memory_order_acquire)) << "Expected argument marker not set";
}

Coro<size_t> skynet_join(size_t my_num, size_t remaining) {
    if (remaining == 1) co_return my_num;
    std::vector<JoinHandle<size_t>> handles;
    for (size_t i = 0; i < 10; ++i)
        handles.emplace_back(
            coro::spawn(skynet_join(my_num + i*(remaining/10), remaining/10))
                .submit()
        );
    size_t sum = 0;
    for (auto &h : handles) {
        sum += co_await h;
    }
    size_t exp = SkynetLookup::instance[my_num, remaining];
    CO_ASSERT_EQ(sum, exp)
        << sum << " != " << exp << " (my_num = " << my_num << ", remaining = " << remaining << ")";
    co_return sum;
}

TEST(SkynetJoinTest, SingleThreaded) {
    size_t result = Runtime(std::in_place_type<SingleThreadedExecutor>).block_on(skynet_join(0, 1000000));
    EXPECT_EQ(result, (SkynetLookup::instance[0, 1000000]));
}

TEST(SkynetJoinTest, Sharing) {
    size_t result = Runtime(std::in_place_type<WorkSharingExecutor>).block_on(skynet_join(0, 1000000));
    EXPECT_EQ(result, (SkynetLookup::instance[0, 1000000]));
}

TEST(SkynetJoinTest, Stealing) {
    size_t result = Runtime(std::in_place_type<WorkStealingExecutor>).block_on(skynet_join(0, 1000000));
    EXPECT_EQ(result, (SkynetLookup::instance[0, 1000000]));
}

Coro<size_t> skynet_joinset(size_t my_num, size_t remaining) {
    if (remaining == 1) co_return my_num;
    JoinSet<size_t> js;
    for (size_t i = 0; i < 10; ++i)
        js.spawn(skynet_joinset(my_num + i*(remaining/10), remaining/10));
    size_t sum = 0;
    while (auto item = co_await next(js)) {
        sum += item .value();
    }
    size_t exp = SkynetLookup::instance[my_num, remaining];
    CO_ASSERT_EQ(sum, exp)
        << sum << " != " << exp << " (my_num = " << my_num << ", remaining = " << remaining << ")";
    co_return sum;
}

TEST(SkynetJoinSetTest, SingleThreaded) {
    size_t result = Runtime(std::in_place_type<SingleThreadedExecutor>).block_on(skynet_joinset(0, 1000000));
    EXPECT_EQ(result, (SkynetLookup::instance[0, 1000000]));
}

TEST(SkynetJoinSetTest, Sharing) {
    size_t result = Runtime(std::in_place_type<WorkSharingExecutor>).block_on(skynet_joinset(0, 1000000));
    EXPECT_EQ(result, (SkynetLookup::instance[0, 1000000]));
}

TEST(SkynetJoinSetTest, Stealing) {
    size_t result = Runtime(std::in_place_type<WorkStealingExecutor>).block_on(skynet_joinset(0, 1000000));
    EXPECT_EQ(result, (SkynetLookup::instance[0, 1000000]));
}

template<std::size_t... Is>
Coro<size_t> skynet_mixed1(size_t my_num, size_t remaining, std::index_sequence<Is...> seq) {
    if (remaining == 1) co_return my_num;
    auto results = co_await join(skynet_mixed1(my_num + Is*(remaining/10), remaining/10, seq)...);
    size_t sum = (std::get<Is>(results) + ...);
    size_t exp = SkynetLookup::instance[my_num, remaining];
    CO_ASSERT_EQ(sum, exp)
        << sum << " != " << exp << " (my_num = " << my_num << ", remaining = " << remaining << ")";
    co_return sum;
}

Coro<size_t> skynet_mixed0(size_t my_num, size_t remaining) {
    if (remaining == 1) co_return my_num;
    JoinSet<size_t> js;
    for (size_t i = 0; i < 10; ++i)
        js.spawn(skynet_mixed1(my_num + i*(remaining/10), remaining/10, std::make_index_sequence<10>{}));
    size_t sum = 0;
    while (auto item = co_await next(js)) {
        sum += item .value();
    }
    size_t exp = SkynetLookup::instance[my_num, remaining];
    CO_ASSERT_EQ(sum, exp)
        << sum << " != " << exp << " (my_num = " << my_num << ", remaining = " << remaining << ")";
    co_return sum;
}

TEST(SkynetMixedTest, SingleThreaded) {
    size_t result = Runtime(std::in_place_type<SingleThreadedExecutor>).block_on(skynet_mixed0(0, 1000000));
    EXPECT_EQ(result, (SkynetLookup::instance[0, 1000000]));
}

TEST(SkynetMixedTest, Sharing) {
    size_t result = Runtime(std::in_place_type<WorkSharingExecutor>).block_on(skynet_mixed0(0, 1000000));
    EXPECT_EQ(result, (SkynetLookup::instance[0, 1000000]));
}

TEST(SkynetMixedTest, Stealing) {
    size_t result = Runtime(std::in_place_type<WorkStealingExecutor>).block_on(skynet_mixed0(0, 1000000));
    EXPECT_EQ(result, (SkynetLookup::instance[0, 1000000]));
}