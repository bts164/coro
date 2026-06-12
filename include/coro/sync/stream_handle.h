#pragma once

// Internal — consumer end of a bounded channel backing a spawned stream.
// Returned by Runtime::spawn(stream) and SpawnBuilder::spawn(stream).

#include <coro/detail/context.h>
#include <coro/detail/poll_result.h>
#include <coro/detail/waker.h>
#include <coro/detail/task.h>
#include <exception>
#include <memory>
#include <optional>
#include <coro/detail/mutex.h>
#include <queue>

namespace coro {

namespace detail {

// Shared state between the StreamDriver (producer) and StreamHandle (consumer).
// Protected by mutex so it is safe for the multi-threaded executor.
template<typename T>
struct Channel {
    detail::Mutex          mutex;
    std::queue<T>          buffer;
    std::size_t            capacity;
    bool                   closed   = false;
    std::exception_ptr     exception;
    std::shared_ptr<Waker> producer_waker;  // wake when buffer has space
    std::shared_ptr<Waker> consumer_waker;  // wake when buffer has items or stream is closed

    explicit Channel(std::size_t cap) : capacity(cap) {}
};

} // namespace detail

/**
 * @brief Consumer end of a bounded channel backed by a background `StreamDriver` task.
 *
 * Returned by `Runtime::spawn(stream)` and `SpawnBuilder::spawn(stream)`. Satisfies `Stream<T>`.
 *
 * The background driver polls the original stream and pushes items into the channel.
 * `StreamHandle::poll_next()` dequeues items from the channel, blocking under backpressure
 * until the driver produces more.
 *
 * `[[nodiscard]]` — discarding it drops the consumer end; items produced by the driver
 * will be lost.
 *
 * @tparam T The item type yielded by the stream.
 */
template<typename T>
class [[nodiscard]] StreamHandle {
public:
    using ItemType = T;

    StreamHandle() = default;

    explicit StreamHandle(std::shared_ptr<detail::Channel<T>> channel,
                          detail::OwnedTask driver = {})
        : m_channel(std::move(channel)), m_driver(std::move(driver)) {}

    // Cancel the driver task if the handle is dropped before the stream is exhausted.
    // Mirrors JoinHandle's cancel-on-destroy so the producer doesn't run indefinitely
    // after the consumer is gone, and ensures the driver task is never left with no owner.
    ~StreamHandle() {
        if (auto task = m_driver.get())
            task->cancel_task();
    }

    StreamHandle(const StreamHandle&)            = delete;
    StreamHandle& operator=(const StreamHandle&) = delete;

    StreamHandle(StreamHandle&&) noexcept            = default;
    StreamHandle& operator=(StreamHandle&&) noexcept = default;

    PollResult<std::optional<T>> poll_next(detail::Context& ctx) {
        if (!m_channel) return std::optional<T>(std::nullopt);

        std::unique_lock lock(m_channel->mutex);

        if (!m_channel->buffer.empty()) {
            T item = std::move(m_channel->buffer.front());
            m_channel->buffer.pop();
            auto to_wake = std::move(m_channel->producer_waker);
            m_channel->producer_waker.reset();
            lock.unlock();
            if (to_wake) to_wake->wake();
            return std::optional<T>(std::move(item));
        }

        if (m_channel->closed) {
            if (m_channel->exception) return PollError(m_channel->exception);
            return std::optional<T>(std::nullopt);
        }

        m_channel->consumer_waker = ctx.getWaker()->clone();
        return PollPending;
    }

private:
    std::shared_ptr<detail::Channel<T>> m_channel;
    // Lifetime anchor for the background StreamDriver task (mirrors OwnedTask in JoinHandle).
    // Without this, the work-stealing executor's raw-pointer queue has no persistent owner,
    // and the task can be freed before a worker calls shared_from_this() on it.
    detail::OwnedTask m_driver;
};

} // namespace coro
