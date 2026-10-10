#pragma once
// IsrTrigger: calls a function "from an interrupt", a number of times at a fixed
// period, so one test body serves the board and the host.
//
//   on the board   a Pico SDK alarm. Its callback runs in the hardware timer's
//                  interrupt handler, so the function really does preempt the
//                  executor, with interrupts as the only concurrency.
//   on the host    a thread that sleeps and calls the function. That is real
//                  concurrency, which the stubbed spin lock (a std::mutex, see
//                  test/pico/stub/hardware/sync.h) makes well defined.
//
//   IsrTrigger t(5ms, 3, [&ev](int i) { ev.signal_from_isr(); });   // i = 0, 1, 2
//   rt.block_on(...);
//   // ~IsrTrigger waits for the last call, like std::thread::join().
//
// The function must be safe to run in an interrupt handler: no allocation, no
// blocking, nothing that takes a lock the interrupted code may hold.

#include <chrono>
#include <functional>
#include <utility>

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE

#include <gtest/gtest.h>
#include <pico/time.h>
#include <pico/platform.h>

class IsrTrigger {
public:
    IsrTrigger(std::chrono::microseconds period, int count, std::function<void(int)> fn)
        : m_fn(std::move(fn)), m_count(count), m_period_us(period.count()) {
        if (m_count <= 0) return;
        // fire_if_past: if the period has already gone by when the alarm is set,
        // the first call happens from here instead of being lost.
        const alarm_id_t id = add_alarm_in_us(
            static_cast<uint64_t>(m_period_us), &IsrTrigger::fire, this, true);
        if (id < 0) {
            // No alarm slot left: nothing will ever call fn. Do not wait for it.
            ADD_FAILURE() << "IsrTrigger: add_alarm_in_us() failed";
            m_fired = m_count;
        }
    }

    IsrTrigger(const IsrTrigger&)            = delete;
    IsrTrigger& operator=(const IsrTrigger&) = delete;

    // Waits for the last call. The alarm holds a pointer to this object, so it must
    // not outlive it.
    ~IsrTrigger() {
        while (m_fired < m_count) tight_loop_contents();
    }

private:
    // Runs in the timer interrupt handler.
    static int64_t fire(alarm_id_t, void* self_ptr) {
        auto* self = static_cast<IsrTrigger*>(self_ptr);
        const int i = self->m_fired;
        self->m_fn(i);
        // Race (benign): the destructor reads m_fired with interrupts enabled. It
        // is one aligned 32-bit word, written only here, so the read sees either
        // the old or the new count. Written after fn returns, so the destructor
        // cannot finish while fn is still running.
        self->m_fired = i + 1;
        // Positive: run again that long after this returns. Zero: done.
        return i + 1 < self->m_count ? self->m_period_us : 0;
    }

    std::function<void(int)> m_fn;
    int                      m_count;
    int64_t                  m_period_us;
    volatile int             m_fired = 0;
};

#else  // --- host: a thread stands in for the interrupt ---

#include <thread>

class IsrTrigger {
public:
    IsrTrigger(std::chrono::microseconds period, int count, std::function<void(int)> fn)
        : m_thread([period, count, fn = std::move(fn)]() {
              for (int i = 0; i < count; ++i) {
                  std::this_thread::sleep_for(period);
                  fn(i);
              }
          }) {}

    IsrTrigger(const IsrTrigger&)            = delete;
    IsrTrigger& operator=(const IsrTrigger&) = delete;

    ~IsrTrigger() { m_thread.join(); }

private:
    std::thread m_thread;
};

#endif
