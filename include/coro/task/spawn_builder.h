#pragma once

#include <coro/runtime/executor.h>
#include <coro/task/join_handle.h>
#include <coro/stream.h>
#include <coro/sync/stream_handle.h>
#include <coro/detail/task.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace coro {

/**
 * @brief Builder for submitting a @ref Future as a background task.
 *
 * Returned by `Runtime::spawn(future)` and the free `spawn(future)` function.
 * Configure the task with optional setters, then call `submit()` to enqueue it
 * and receive a @ref JoinHandle.
 *
 * `[[nodiscard]]` — discarding the builder silently drops the future without submitting it.
 *
 * @tparam F A type satisfying @ref Future.
 */
template<Future F>
class [[nodiscard]] SpawnBuilder {
public:
    using OutputType = typename F::OutputType;

    explicit SpawnBuilder(F future, Executor* executor)
        : m_future(std::move(future)), m_executor(executor) {}

    /// @brief Sets an optional name for the task (for diagnostics).
    SpawnBuilder& name(std::string n) {
        m_name = std::move(n);
        return *this;
    }

    /// @brief Enqueues the task on the executor and returns a @ref JoinHandle.
    /// The builder must not be used again after this call.
    JoinHandle<OutputType> submit() {
        auto impl = std::make_shared<detail::TaskImpl<F>>(std::move(m_future));
        impl->name = std::move(m_name);
        std::shared_ptr<detail::TaskState<OutputType>> state = impl;
        std::shared_ptr<detail::TaskBase> task = impl;  // aliased; copied before impl is moved
        if (m_executor)
            m_executor->schedule(std::shared_ptr<detail::TaskBase>(std::move(impl)));
        return JoinHandle<OutputType>(std::move(state), std::move(task));
    }

private:
    F           m_future;
    Executor*   m_executor;
    std::string m_name;
};


namespace detail {

// StreamDriver<S> — internal Future<void> that drives a stream and pushes items
// through a bounded Channel. Runs as a background Task scheduled by the executor.
// Backpressure: when the channel buffer is full, the driver parks itself and waits
// for the consumer (StreamHandle) to make space.
template<Stream S>
struct StreamDriver {
    using OutputType = void;
    using ItemType   = typename S::ItemType;

    S                                        m_stream;
    std::shared_ptr<Channel<ItemType>>       m_channel;
    std::optional<ItemType>                  m_pending;  // item buffered while channel was full

    PollResult<void> poll(Context& ctx) {
        // Flush pending item (backpressure recovery).
        if (m_pending.has_value()) {
            std::unique_lock lock(m_channel->mutex);
            if (m_channel->buffer.size() < m_channel->capacity) {
                m_channel->buffer.push(std::move(*m_pending));
                m_pending.reset();
                auto to_wake = std::move(m_channel->consumer_waker);
                m_channel->consumer_waker.reset();
                lock.unlock();
                if (to_wake) to_wake->wake();
                // Fall through to poll stream again.
            } else {
                m_channel->producer_waker = ctx.getWaker()->clone();
                return PollPending;
            }
        }

        // Drain the stream into the channel.
        while (true) {
            auto result = m_stream.poll_next(ctx);

            if (result.isPending()) return PollPending;

            if (result.isError()) {
                std::unique_lock lock(m_channel->mutex);
                m_channel->exception = result.error();
                m_channel->closed    = true;
                auto to_wake = std::move(m_channel->consumer_waker);
                m_channel->consumer_waker.reset();
                lock.unlock();
                if (to_wake) to_wake->wake();
                return PollReady;
            }

            auto opt = std::move(result).value();
            if (!opt.has_value()) {
                // Stream exhausted.
                std::unique_lock lock(m_channel->mutex);
                m_channel->closed = true;
                auto to_wake = std::move(m_channel->consumer_waker);
                m_channel->consumer_waker.reset();
                lock.unlock();
                if (to_wake) to_wake->wake();
                return PollReady;
            }

            // Got an item — push to buffer or park under backpressure.
            std::unique_lock lock(m_channel->mutex);
            if (m_channel->buffer.size() < m_channel->capacity) {
                m_channel->buffer.push(std::move(*opt));
                auto to_wake = std::move(m_channel->consumer_waker);
                m_channel->consumer_waker.reset();
                lock.unlock();
                if (to_wake) to_wake->wake();
                // Keep polling stream.
            } else {
                m_pending                = std::move(opt);
                m_channel->producer_waker = ctx.getWaker()->clone();
                return PollPending;
            }
        }
    }
};

} // namespace detail


/**
 * @brief Builder for spawning a @ref Stream as a background task.
 *
 * Returned by `Runtime::spawn(stream)` and the free `spawn(stream)` function.
 * The stream runs as a `StreamDriver` task that pushes items into a bounded channel;
 * the consumer reads from the returned @ref StreamHandle.
 *
 * `[[nodiscard]]` — discarding the builder silently drops the stream without submitting it.
 *
 * @tparam S A type satisfying @ref Stream.
 */
template<Stream S>
class [[nodiscard]] StreamSpawnBuilder {
public:
    using ItemType = typename S::ItemType;

    explicit StreamSpawnBuilder(S stream, Executor* executor)
        : m_stream(std::move(stream)), m_executor(executor) {}

    /// @brief Sets an optional name for the task (for diagnostics).
    StreamSpawnBuilder& name(std::string n) {
        m_name = std::move(n);
        return *this;
    }

    /// @brief Sets the bounded channel capacity (default: 64 items).
    /// The stream driver parks under backpressure when the buffer is full.
    StreamSpawnBuilder& buffer(std::size_t size) {
        m_buffer_size = size;
        return *this;
    }

    /// @brief Creates a bounded channel, schedules the stream driver, and returns a @ref StreamHandle.
    StreamHandle<ItemType> submit() {
        auto channel = std::make_shared<detail::Channel<ItemType>>(m_buffer_size);
        if (m_executor) {
            using Driver = detail::StreamDriver<S>;
            auto impl = std::make_shared<detail::TaskImpl<Driver>>(
                Driver{std::move(m_stream), channel, std::nullopt});
            impl->name = std::move(m_name);
            m_executor->schedule(std::shared_ptr<detail::TaskBase>(std::move(impl)));
        }
        return StreamHandle<ItemType>(std::move(channel));
    }

private:
    S           m_stream;
    Executor*   m_executor;
    std::string m_name;
    std::size_t m_buffer_size = 64;
};

} // namespace coro
