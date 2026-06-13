#pragma once
// Traits types for TYPED_TEST_SUITE parameterisation across all executor types.
//
// Each traits struct owns a coro::Runtime constructed with a specific executor.
// A fresh Runtime (and executor) is created per test because TYPED_TEST_SUITE
// instantiates a new fixture object for each test case.
//
// Under CORO_PICO only CurrentThreadTraits/AllExecutors are defined;
// the other three executors are not available in that build.

#include <gtest/gtest.h>
#include <coro/runtime/runtime.h>

#ifdef CORO_PICO

struct CurrentThreadTraits {
    coro::Runtime rt;
};

using AllExecutors = testing::Types<CurrentThreadTraits>;

#else  // --- desktop: all four executors ---

#include <coro/runtime/current_thread_executor.h>
#include <coro/runtime/work_sharing_executor.h>
#include <chrono>

struct SingleThreadedTraits {
    coro::Runtime rt{std::size_t{1}};
};

struct WorkStealingTraits {
    coro::Runtime rt{std::size_t{4}};
};

struct WorkSharingTraits {
    coro::Runtime rt{std::in_place_type<coro::WorkSharingExecutor>, std::size_t{4}};
};

struct CurrentThreadTraits {
    coro::Runtime rt{
        std::in_place_type<coro::CurrentThreadExecutor>,
        []() -> uint64_t {
            static const auto start = std::chrono::steady_clock::now();
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count());
        },
        []() {}
    };
};

using AllExecutors = testing::Types<
    SingleThreadedTraits,
    WorkStealingTraits,
    WorkSharingTraits,
    CurrentThreadTraits>;

#endif  // CORO_PICO
