#pragma once
// Traits types for TYPED_TEST_SUITE parameterisation across all executor types.
//
// Each traits struct owns a coro::Runtime constructed with a specific executor.
// A fresh Runtime (and executor) is created per test because TYPED_TEST_SUITE
// instantiates a new fixture object for each test case.
//
// Under CORO_PICO only CurrentThreadTraits/AllExecutors are defined;
// the other executors are not available in that build.

#include <gtest/gtest.h>
#include <coro/runtime/runtime.h>

#ifdef CORO_PICO

struct CurrentThreadTraits {
    coro::Runtime rt;
};

using AllExecutors = testing::Types<CurrentThreadTraits>;

#else  // --- desktop: all three executors ---

#include <coro/runtime/work_sharing_executor.h>

struct WorkStealingTraits {
    coro::Runtime rt{std::size_t{4}};
};

struct WorkSharingTraits {
    coro::Runtime rt{std::in_place_type<coro::WorkSharingExecutor>, std::size_t{4}};
};

// Runtime(1) builds a CurrentThreadExecutor that parks in the epoll IoDriver.
struct CurrentThreadTraits {
    coro::Runtime rt{std::size_t{1}};
};

using AllExecutors = testing::Types<
    CurrentThreadTraits,
    WorkStealingTraits,
    WorkSharingTraits>;

#endif  // CORO_PICO
