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

// SpawnBuilder<F> — builder returned by Runtime::spawn(future) and the free spawn(future).
// Configure the task with optional setters, then call submit() to enqueue it and get a handle.
// [[nodiscard]] because discarding it silently drops the future without submitting anything.
template<Future F>
class [[nodiscard]] SpawnBuilder {
public:
    using OutputType = typename F::OutputType;

    explicit SpawnBuilder(F future, Executor* executor)
        : m_future(std::move(future)), m_executor(executor) {}

    SpawnBuilder& name(std::string n) {
        m_name = std::move(n);
        return *this;
    }

    // Submits the task to the executor and returns a JoinHandle.
    // After submit() the builder must not be used again.
    JoinHandle<OutputType> submit() {
        auto state = std::make_shared<detail::TaskState<OutputType>>();
        if (m_executor)
            m_executor->schedule(
                std::make_unique<detail::Task>(std::move(m_future), state));
        return JoinHandle<OutputType>(std::move(state));
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


// StreamSpawnBuilder<S> — builder returned by Runtime::spawn(stream) and free spawn(stream).
// buffer() sets the bounded channel capacity (default: 64); submit() enqueues the task.
template<Stream S>
class [[nodiscard]] StreamSpawnBuilder {
public:
    using ItemType = typename S::ItemType;

    explicit StreamSpawnBuilder(S stream, Executor* executor)
        : m_stream(std::move(stream)), m_executor(executor) {}

    StreamSpawnBuilder& name(std::string n) {
        m_name = std::move(n);
        return *this;
    }

    StreamSpawnBuilder& buffer(std::size_t size) {
        m_buffer_size = size;
        return *this;
    }

    // Creates a bounded channel, wraps the stream in a StreamDriver Task, schedules it,
    // and returns the consumer end as a StreamHandle.
    StreamHandle<ItemType> submit() {
        auto channel = std::make_shared<detail::Channel<ItemType>>(m_buffer_size);
        if (m_executor) {
            m_executor->schedule(std::make_unique<detail::Task>(
                detail::StreamDriver<S>{std::move(m_stream), channel, std::nullopt}));
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
