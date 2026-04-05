#pragma once

#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>

namespace coro::test {

/**
 * @brief Accumulates a failure message and throws on destruction.
 *
 * Constructed by CO_ASSERT_* macros. Supports streaming additional context:
 *
 *   CO_ASSERT_EQ(a, b) << "iteration " << i;
 *
 * The throw propagates out of the coroutine; Runtime::block_on() re-throws it
 * so gtest sees it as a test failure.
 *
 * The destructor is noexcept(false). If a prior exception is already unwinding
 * the stack (std::uncaught_exceptions() > 0) the throw is suppressed to avoid
 * std::terminate — the first failure is already in flight.
 */
class CoAssertion {
public:
    CoAssertion(bool failed, const char* expr, const char* file, int line)
        : m_failed(failed)
    {
        if (failed)
            m_msg << file << ":" << line << ": CO_ASSERT(" << expr << ") failed";
    }

    // Non-copyable, non-movable — only lives as a temporary in a statement.
    CoAssertion(const CoAssertion&) = delete;
    CoAssertion& operator=(const CoAssertion&) = delete;

    template<typename T>
    CoAssertion& operator<<(const T& val) {
        if (m_failed) m_msg << " " << val;
        return *this;
    }

    ~CoAssertion() noexcept(false) {
        if (!m_failed) return;
        if (std::uncaught_exceptions() > 0) return; // already unwinding
        ADD_FAILURE() << m_msg.str();
        std::cerr << m_msg.str() << "\n" <<std::flush;
        std::terminate();
    }

private:
    bool               m_failed;
    std::ostringstream m_msg;
};

} // namespace coro::test

/// @brief Asserts that @p cond is true inside a coroutine. Supports `<< msg`.
#define CO_ASSERT(cond) \
    ::coro::test::CoAssertion(!(cond), #cond, __FILE__, __LINE__)

/// @brief Asserts equality inside a coroutine. Supports `<< msg`.
#define CO_ASSERT_EQ(a, b) \
    ::coro::test::CoAssertion(!((a) == (b)), #a " == " #b, __FILE__, __LINE__)

/// @brief Asserts inequality inside a coroutine. Supports `<< msg`.
#define CO_ASSERT_NE(a, b) \
    ::coro::test::CoAssertion((a) == (b), #a " != " #b, __FILE__, __LINE__)

/// @brief Asserts a < b inside a coroutine. Supports `<< msg`.
#define CO_ASSERT_LT(a, b) \
    ::coro::test::CoAssertion(!((a) < (b)), #a " < " #b, __FILE__, __LINE__)
