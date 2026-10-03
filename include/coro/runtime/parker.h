#pragma once

// How an executor thread waits for outside events between batches of tasks.
// See doc/design/io_driver.md, "Parker".

#include <chrono>
#include <functional>
#include <optional>
#include <utility>

namespace coro {

/**
 * @brief Blocks an executor thread until outside events arrive, and lets other threads
 * cut that wait short (tokio's `Park` trait).
 *
 * The executor calls park() when it has run what is ready; wakes from other threads call
 * unpark(). Implementations:
 * - @ref IoDriverParker (desktop): park() turns the epoll driver, so the wait doubles as
 *   I/O polling.
 * - @ref PollingParker (Pico): park() calls a platform poll function once and never
 *   blocks.
 */
class Parker {
public:
    virtual ~Parker() = default;

    /**
     * @brief Waits up to `max_wait` for outside events (I/O, unpark()).
     *
     * May return early, spuriously, or (for a non-blocking parker) immediately. Only the
     * owning executor's thread calls it.
     *
     * @param max_wait `std::nullopt` = no limit; zero = don't block.
     */
    virtual void park(std::optional<std::chrono::nanoseconds> max_wait) = 0;

    /**
     * @brief Makes a blocked park() return, or the next park() return immediately.
     *
     * Safe from any thread. Publish the work (e.g. push onto the ready queue) BEFORE
     * calling this, so the woken thread can see it.
     */
    virtual void unpark() noexcept = 0;
};

/**
 * @brief A Parker that never blocks: park() calls `poll` once and returns.
 *
 * For targets with no blocking wait, such as Pico W (`poll` = `cyw43_arch_poll()`). The
 * executor loop then busy-polls, exactly as before Parker existed. unpark() has nothing
 * to interrupt and does nothing.
 */
class PollingParker final : public Parker {
public:
    explicit PollingParker(std::function<void()> poll) : m_poll(std::move(poll)) {}

    void park(std::optional<std::chrono::nanoseconds> /*max_wait*/) override { m_poll(); }
    void unpark() noexcept override {}

private:
    std::function<void()> m_poll;
};

} // namespace coro
