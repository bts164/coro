#pragma once

// Platform-portable scheduling-state wrapper.
//
//   Multi-threaded platforms (non-CORO_PICO):
//     TaskBase::scheduling_state and the cancellation flags are read/written
//     from multiple threads (executor worker threads, JoinHandle/StreamHandle
//     destructors called from arbitrary threads), so std::atomic<T> is
//     required.
//
//   CORO_PICO (RP2040 bare-metal):
//     CurrentThreadExecutor is cooperative and single-threaded — the same
//     argument that justifies the no-op Mutex (see detail/mutex.h) and the
//     non-atomic Rc<T>/Weak<T> (see detail/rc.h). RelaxedState<T> exposes
//     the same load/store/compare_exchange_strong surface std::atomic<T>
//     does (ignoring the std::memory_order arguments) backed by a plain T,
//     so call sites in task.cpp/current_thread_executor.cpp/detail/task.h
//     need zero changes beyond the field's declared type.
//
// Usage:
//   #include <coro/detail/relaxed_state.h>
//   coro::detail::RelaxedState<SchedulingState> scheduling_state{SchedulingState::Idle};

#ifdef CORO_PICO

#include <atomic>  // for std::memory_order

namespace coro::detail {

template<typename T>
class RelaxedState {
public:
    RelaxedState() = default;
    RelaxedState(T v) : m_value(v) {}

    T load(std::memory_order = std::memory_order_relaxed) const { return m_value; }
    void store(T v, std::memory_order = std::memory_order_relaxed) { m_value = v; }

    bool compare_exchange_strong(T& expected, T desired,
                                  std::memory_order = std::memory_order_relaxed,
                                  std::memory_order = std::memory_order_relaxed) {
        if (m_value != expected) {
            expected = m_value;
            return false;
        }
        m_value = desired;
        return true;
    }

private:
    T m_value{};
};

} // namespace coro::detail

#else  // ---- multi-threaded targets, std::atomic --------------------------

#include <atomic>

namespace coro::detail {
    template<typename T> using RelaxedState = std::atomic<T>;
} // namespace coro::detail

#endif // CORO_PICO
