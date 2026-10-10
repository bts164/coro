// Runner for the on-target test firmware. See doc/design/on_target_tests.md.
//
// Protocol with test/target/run_on_target.py, one text line each way:
//
//   device: CORO_TEST_READY                    (once a second, until told to run)
//   host:   RUN [gtest filter]
//   device: CORO_TEST_BEGIN heap_total=N heap_used=N
//   device: ...gtest's normal output...
//           with CORO_TEST_HEAP used=N before each test
//   device: CORO_TEST_END rc=N heap_used=N heap_peak=N
//
// The device then goes back to announcing CORO_TEST_READY, so the tests can be
// run again, with another filter, without flashing.

#include <gtest/gtest.h>

#include <pico/stdlib.h>

#include <malloc.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Set by the Pico SDK's linker script: the heap runs from the end of .bss up to
// __StackLimit.
extern "C" {
extern char __StackLimit;
extern char __bss_end__;
}

namespace {

std::size_t heap_total() {
    return static_cast<std::size_t>(&__StackLimit - &__bss_end__);
}

// Bytes in live allocations.
std::size_t heap_used() {
    return static_cast<std::size_t>(mallinfo().uordblks);
}

// Bytes the allocator has ever taken from the heap region. newlib does not give
// memory back, so this is the high-water mark.
std::size_t heap_peak() {
    return static_cast<std::size_t>(mallinfo().arena);
}

// Reads one line into `buf`, without its line ending. Returns false if no complete
// line arrived within `timeout_us`.
bool read_line(char* buf, std::size_t capacity, std::uint32_t timeout_us) {
    const absolute_time_t deadline = make_timeout_time_us(timeout_us);
    std::size_t len = 0;
    while (!time_reached(deadline)) {
        const int c = getchar_timeout_us(10'000);
        if (c == PICO_ERROR_TIMEOUT) continue;
        if (c == '\r') continue;
        if (c == '\n') {
            buf[len] = '\0';
            return true;
        }
        if (len + 1 < capacity) buf[len++] = static_cast<char>(c);
    }
    return false;
}

// Reports the heap in use before each test. When a test takes the board down by
// running out of memory, the last of these lines shows how much it had to work with.
class HeapReporter : public testing::EmptyTestEventListener {
    void OnTestStart(const testing::TestInfo&) override {
        std::printf("CORO_TEST_HEAP used=%u\n", static_cast<unsigned>(heap_used()));
    }
};

} // namespace

int main() {
    stdio_init_all();

    // Every TEST registered itself before main() ran, so this is what the suite
    // costs before a single test has run.
    const std::size_t used_at_start = heap_used();

    testing::InitGoogleTest();
    GTEST_FLAG_SET(color, "no");
    // gtest owns and deletes the listener.
    testing::UnitTest::GetInstance()->listeners().Append(new HeapReporter);

    for (;;) {
        std::printf("CORO_TEST_READY\n");
        std::fflush(stdout);

        char line[200];
        if (!read_line(line, sizeof line, 1'000'000)) continue;
        if (std::strncmp(line, "RUN", 3) != 0) continue;

        const char* filter = line + 3;
        while (*filter == ' ') ++filter;
        GTEST_FLAG_SET(filter, *filter != '\0' ? filter : "*");

        std::printf("CORO_TEST_BEGIN heap_total=%u heap_used=%u\n",
                    static_cast<unsigned>(heap_total()),
                    static_cast<unsigned>(used_at_start));

        const int rc = RUN_ALL_TESTS();

        std::printf("CORO_TEST_END rc=%d heap_used=%u heap_peak=%u\n", rc,
                    static_cast<unsigned>(heap_used()),
                    static_cast<unsigned>(heap_peak()));
        std::fflush(stdout);
    }
}
