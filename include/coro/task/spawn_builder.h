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
 * @brief Builder for configuring a background task before spawning it.
 *
 * Returned by `Runtime::build_task()` and the free `build_task()` function.
 * Configure the task with optional setters, then call `spawn(future)` or
 * `spawn(stream)` to enqueue it and receive a @ref JoinHandle or @ref StreamHandle.
 *
 * `[[nodiscard]]` — discarding the builder silently drops the configuration without
 * spawning anything.
 */
class [[nodiscard]] SpawnBuilder {
public:
    explicit SpawnBuilder(Executor* executor) : m_executor(executor) {}

    /// @brief Sets an optional name for the task (for diagnostics).
    SpawnBuilder& name(std::string n) {
        m_name = std::move(n);
        return *this;
    }

    /// @brief Sets the bounded channel capacity for stream tasks (default: 64 items).
    SpawnBuilder& buffer(std::size_t size) {
        m_buffer_size = size;
        return *this;
    }

    /// @brief Enqueues `future` as a background task and returns a @ref JoinHandle.
    template<Future F>
    JoinHandle<typename F::OutputType> spawn(F future) {
        using T = typename F::OutputType;
        auto impl = std::make_shared<detail::TaskImpl<F>>(std::move(future));
        impl->name = std::move(m_name);
        std::shared_ptr<detail::TaskState<T>> state = impl;
        // Aliased shared_ptr to the same TaskImpl allocation — wrapped in OwnedTask
        // to enforce the single-owner invariant (see doc/task_ownership.md).
        detail::OwnedTask owned{std::shared_ptr<detail::TaskBase>(impl)};
        if (m_executor)
            m_executor->schedule(std::shared_ptr<detail::TaskBase>(std::move(impl)));
        return JoinHandle<T>(std::move(state), std::move(owned));
    }

    /// @brief Creates a bounded channel, schedules the stream driver, and returns a @ref StreamHandle.
    template<Stream S>
    StreamHandle<typename S::ItemType> spawn(S stream) {
        auto channel = std::make_shared<detail::Channel<typename S::ItemType>>(m_buffer_size);
        if (m_executor) {
            using Driver = detail::StreamDriver<S>;
            auto impl = std::make_shared<detail::TaskImpl<Driver>>(
                Driver{std::move(stream), channel, std::nullopt});
            impl->name = std::move(m_name);
            m_executor->schedule(std::shared_ptr<detail::TaskBase>(std::move(impl)));
        }
        return StreamHandle<typename S::ItemType>(std::move(channel));
    }

private:
    Executor*   m_executor;
    std::string m_name;
    std::size_t m_buffer_size = 64;
};

} // namespace coro
