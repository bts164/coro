#pragma once

// Trace points for the runtime's own timeline: task polls and I/O driver turns
// (doc/design/tracing.md). A thread the runtime owns is in a zone for all the time it
// is awake, and in none while it sleeps waiting for work or events, so a gap in its
// row is that idle time and nothing else. Private to the library; not installed.
//
// Every type here is a scope object that opens a zone in its constructor and closes
// it in its destructor, so a zone begins and ends in one function on one thread.
// The one exception is AwakeZone, which a SleepScope closes and reopens; Tracy
// zones must nest, so that is only allowed while it is the innermost zone (see t_awake).
// With no backend selected (CORO_TRACING=none, the default) they are all empty and
// the trace points compile to nothing.
//
// Proof of concept: the Tracy backend only, and not every trace point in the design.

#include <coro/detail/task.h>

#include <cstddef>
#include <cstdint>

#ifdef CORO_TRACE_TRACY

#include <tracy/Tracy.hpp>

#include <cstdio>
#include <optional>
#include <utility>

namespace coro::detail::trace {

class AwakeZone;

/// The calling thread's AwakeZone, which a SleepScope may close: null on a thread the
/// runtime doesn't own, and for the length of a task poll or blocking job, where the
/// zone is not the innermost one and the time belongs to the task anyway.
inline thread_local AwakeZone* t_awake = nullptr;

/// @brief Labels the calling thread's row, as "<kind>-<index>".
inline void thread_name(const char* kind, int index) {
    char name[32];
    std::snprintf(name, sizeof(name), "%s-%d", kind, index);
    tracy::SetThreadName(name);   // copies the string
}

/// @brief Labels the calling thread's row with `name`, for threads that have no index.
inline void thread_name(const char* name) { tracy::SetThreadName(name); }

/// @brief Open for as long as a runtime-owned thread is awake: from the start of its
/// loop to its end, except while a SleepScope has it closed. Everything else the thread
/// records nests inside it.
class AwakeZone {
public:
    AwakeZone() : m_prev(std::exchange(t_awake, this)) { begin(); }
    ~AwakeZone() { t_awake = m_prev; }
    AwakeZone(const AwakeZone&)            = delete;
    AwakeZone& operator=(const AwakeZone&) = delete;

    void begin() { m_zone.emplace(&kLocation); }
    void end() { m_zone.reset(); }

private:
    static constexpr tracy::SourceLocationData kLocation{"awake", "AwakeZone", __FILE__, __LINE__, 0};
    AwakeZone* m_prev;   // a loop run from inside a task poll nests in it
    std::optional<tracy::ScopedZone> m_zone;
};

/// @brief Around a call that sleeps until there is work or an event (a condvar wait,
/// a blocking poller call): closes the thread's AwakeZone for the duration, so the
/// sleep is a gap in the row. Not for mutex locks: contention is overhead worth
/// seeing, so it stays inside the zone. Does nothing inside a task poll or blocking job.
class SleepScope {
public:
    SleepScope() : m_awake(t_awake) {
        if (m_awake) m_awake->end();
    }
    ~SleepScope() {
        if (m_awake) m_awake->begin();
    }
    SleepScope(const SleepScope&)            = delete;
    SleepScope& operator=(const SleepScope&) = delete;

private:
    AwakeZone* m_awake;
};

/// @brief One poll of a task, shown under the task's name, from the call of poll()
/// until it returns, whatever the task does inside (blocking included).
///
/// The zone must stay named after the task (SpawnBuilder::name()): the Tracy
/// statistics and find-zone views group polls by zone name, which is how a task's
/// time is found. Don't give it a fixed name, or a prefix.
class PollZone {
public:
    explicit PollZone(const TaskBase& task) : m_zone(&kLocation), m_awake(std::exchange(t_awake, nullptr)) {
        // An unnamed task keeps the location's own name.
        if (!task.name.empty()) m_zone.Name(task.name.data(), task.name.size());
    }
    ~PollZone() { t_awake = m_awake; }
    PollZone(const PollZone&)            = delete;
    PollZone& operator=(const PollZone&) = delete;
    /// @brief Marks the zone if this poll completed the task.
    void finished(bool done) {
        if (done) m_zone.Text("finished", 8);
    }

private:
    static constexpr tracy::SourceLocationData kLocation{"task poll", "PollZone", __FILE__, __LINE__, 0};
    tracy::ScopedZone m_zone;
    AwakeZone* m_awake;   // restored when the poll returns
};

/// @brief One job on a blocking-pool thread: a PollZone for spawn_blocking(), from the
/// call of the callable until it returns, whatever it does inside.
class BlockingJobZone {
public:
    BlockingJobZone() : m_zone(&kLocation), m_awake(std::exchange(t_awake, nullptr)) {}
    ~BlockingJobZone() { t_awake = m_awake; }
    BlockingJobZone(const BlockingJobZone&)            = delete;
    BlockingJobZone& operator=(const BlockingJobZone&) = delete;

private:
    static constexpr tracy::SourceLocationData kLocation{"blocking job", "BlockingJobZone", __FILE__, __LINE__, 0};
    tracy::ScopedZone m_zone;
    AwakeZone* m_awake;   // restored when the job returns
};

/// @brief The poller call of one I/O driver turn, by whether the call can block. With
/// a zero timeout it collects what is already ready and returns at once, as a busy
/// worker does between tasks: an "io poll" zone. A call that can block is a thread
/// with nothing to run, waiting for an event, a timer or an unpark: an "io wait"
/// message marks which thread took the driver, then a SleepScope leaves the wait
/// itself as a gap.
class IoWaitZone {
public:
    /// @param blocking false if the poller is called with a zero timeout
    explicit IoWaitZone(bool blocking) {
        if (blocking) {
            TracyMessageL("io wait");
            m_sleep.emplace();
        } else {
            m_zone.emplace(&kPoll);
        }
    }

private:
    static constexpr tracy::SourceLocationData kPoll{"io poll", "IoWaitZone", __FILE__, __LINE__, 0};
    std::optional<tracy::ScopedZone> m_zone;
    std::optional<SleepScope> m_sleep;
};

/// @brief What follows the wait: waking the tasks whose I/O is ready, and firing timers.
class IoDispatchZone {
public:
    IoDispatchZone() : m_zone(&kLocation) {}
    /// @brief Records how many events and timers the turn dispatched.
    void dispatched(std::size_t count) { m_zone.Value(static_cast<std::uint64_t>(count)); }

private:
    static constexpr tracy::SourceLocationData kLocation{"io dispatch", "IoDispatchZone", __FILE__, __LINE__, 0};
    tracy::ScopedZone m_zone;
};

} // namespace coro::detail::trace

#else // no backend

namespace coro::detail::trace {

inline void thread_name(const char*, int) {}
inline void thread_name(const char*) {}

struct PollZone {
    explicit PollZone(const TaskBase&) {}
    void finished(bool) {}
};

// User-provided constructors, so that a zone nothing else touches is not reported
// as an unused variable.
struct AwakeZone {
    AwakeZone() {}
};

struct SleepScope {
    SleepScope() {}
};

struct BlockingJobZone {
    BlockingJobZone() {}
};

struct IoWaitZone {
    explicit IoWaitZone(bool) {}
};

struct IoDispatchZone {
    void dispatched(std::size_t) {}
};

} // namespace coro::detail::trace

#endif
