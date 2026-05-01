#pragma once

// Lock-free, fixed-capacity, work-stealing run queue.
//
// Direct C++ port of Tokio's
// tokio/src/runtime/scheduler/multi_thread/queue.rs (Apache-2.0 / MIT).
//
// Design summary
// ─────────────
// Each worker owns one Local<T> handle and shares a Steal<T> clone with every
// other worker.  Local is single-threaded; Steal is Send+Sync.
//
// The ring buffer holds LOCAL_QUEUE_CAPACITY slots addressed by wrapping
// uint32_t indices.  A 64-bit atomic `head` packs two uint32_t halves:
//
//   bits [63:32]  steal  — start of any active bulk steal
//   bits [31: 0]  real   — actual consume pointer
//
// When steal == real no steal is in progress.  A stealer claims tasks by
// advancing real (but not steal) via CAS, copies the claimed tasks into its
// own buffer, then advances steal to match real to mark completion.  This
// two-phase protocol ensures only one thread accesses each buffer slot at a
// time, so the slots need no per-element atomics and can store T by value.
//
// The LIFO slot is different: both the owner and stealers may exchange it
// concurrently, so it is stored as std::atomic<T>.  For T = shared_ptr<X>
// this uses the C++20 atomic<shared_ptr> specialization (lock-based but
// correct).
//
// An additional LIFO slot (atomic<T>) caches the most recently pushed task
// so it runs next, improving data locality for message-passing patterns.
//
// When the ring is full, push_or_overflow() spills the second half of the
// ring (+ the new task) to an injection queue, then re-exposes the first half.
// The second half is chosen deliberately so that tasks retrieved from the
// injection queue are not immediately sent back to it.
//
// ABA mitigation: indices are uint32_t (wider than the log2(capacity) bits
// strictly needed), matching Tokio's cfg_has_atomic_u64 path.
//
// T requirements:
//   - Default-constructible to a logically "empty" / null state.
//   - Contextually convertible to bool (false == empty).
//   - Movable.
//   - Either trivially copyable OR std::shared_ptr<X> (for atomic<T> in lifo).

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace coro::detail {

static constexpr std::size_t LOCAL_QUEUE_CAPACITY = 256;
static_assert((LOCAL_QUEUE_CAPACITY & (LOCAL_QUEUE_CAPACITY - 1)) == 0,
              "capacity must be a power of two");
static constexpr std::size_t LOCAL_QUEUE_MASK = LOCAL_QUEUE_CAPACITY - 1;

// ─── index / head helpers ────────────────────────────────────────────────────

namespace lrq_detail {

// Pack two uint32_t indices into one uint64_t head word.
// steal occupies the upper 32 bits; real the lower 32.
inline uint64_t pack(uint32_t steal, uint32_t real) noexcept {
    return (uint64_t{steal} << 32) | uint64_t{real};
}

// Unpack: returns {steal, real}.
inline std::pair<uint32_t, uint32_t> unpack(uint64_t n) noexcept {
    return { uint32_t(n >> 32), uint32_t(n & 0xFFFF'FFFFu) };
}

// Wrapping distance from head to tail (number of occupied slots).
inline uint32_t queue_len(uint32_t head, uint32_t tail) noexcept {
    return tail - head;   // unsigned wrapping is well-defined in C++
}

} // namespace lrq_detail

// ─── shared inner state ───────────────────────────────────────────────────────

// align all members to cache line boundaries to avoid false sharing
template<typename T>
struct LocalRunQueueInner {
    // Packed (steal << 32 | real).  Written by owner (pop) and stealers.
    // RACE: multiple threads CAS this concurrently; AcqRel ordering.
    alignas(64) std::atomic<uint64_t> head{0};

    // Only written by the owner; read by stealers to measure queue size.
    // RACE: owner stores with Release; stealers load with Acquire.
    alignas(64) std::atomic<uint32_t> tail{0};

    // LIFO slot — owner and stealers both exchange this concurrently.
    // Uses std::atomic<T>; for T = shared_ptr<X> this is the C++20
    // atomic<shared_ptr> specialization.
    // RACE: owner and stealers exchange/load this atomically.
    alignas(64) std::atomic<T> lifo{};

    // Ring buffer.  The head/tail protocol guarantees only one thread accesses
    // any given slot at a time, so no per-element atomics are needed.
    // T is stored by value — no boxing or extra allocations.
    alignas(64) std::array<T, LOCAL_QUEUE_CAPACITY> buffer{};
};

template<typename T> class Steal;

// ─── Local ────────────────────────────────────────────────────────────────────

/// Owner (producer) handle.  Must only be used from one thread.
template<typename T>
class Local {
    friend class Steal<T>;
public:
    explicit Local(std::shared_ptr<LocalRunQueueInner<T>> inner) noexcept
        : m_inner(std::move(inner)) {}

    Local(const Local&)            = delete;
    Local& operator=(const Local&) = delete;
    Local(Local&&) noexcept            = default;
    Local& operator=(Local&&) noexcept = default;

    ~Local() {
        assert(!pop()      && "queue not empty on destruction");
        assert(!pop_lifo() && "LIFO slot not empty on destruction");
    }

    // ── capacity queries ──────────────────────────────────────────────────────

    /// Number of tasks currently held (ring + LIFO slot).
    std::size_t len() const noexcept {
        auto [s, head] = lrq_detail::unpack(
            m_inner->head.load(std::memory_order_acquire));
        (void)s;
        uint32_t tail = m_inner->tail.load(std::memory_order_relaxed);
        bool lifo = static_cast<bool>(
            m_inner->lifo.load(std::memory_order_relaxed));
        return lrq_detail::queue_len(head, tail) + (lifo ? 1u : 0u);
    }

    /// Free ring slots available for push_back (accounting for in-progress steals).
    std::size_t remaining_slots() const noexcept {
        auto [steal, r] = lrq_detail::unpack(
            m_inner->head.load(std::memory_order_acquire));
        (void)r;
        uint32_t tail = m_inner->tail.load(std::memory_order_relaxed);
        return LOCAL_QUEUE_CAPACITY - lrq_detail::queue_len(steal, tail);
    }

    std::size_t max_capacity() const noexcept { return LOCAL_QUEUE_CAPACITY; }
    bool        has_tasks()    const noexcept { return len() != 0; }

    // ── LIFO slot ─────────────────────────────────────────────────────────────

    /// Deposit `task` into the LIFO slot.
    /// Returns the displaced task (or a default-constructed T if the slot was empty).
    T push_lifo(T task) noexcept {
        return m_inner->lifo.exchange(std::move(task), std::memory_order_acq_rel);
    }

    /// Consume and return the LIFO-slot task.
    /// Returns a default-constructed (falsy) T if the slot is empty.
    T pop_lifo() noexcept {
        return m_inner->lifo.exchange(nullptr, std::memory_order_acq_rel);
    }

    // ── ring buffer ───────────────────────────────────────────────────────────

    /// Push a single task onto the ring.  Asserts if the ring is full.
    /// Prefer push_or_overflow when the ring may be at capacity.
    void push_back(T task) {
        uint32_t tail = m_inner->tail.load(std::memory_order_relaxed);
        auto [steal, r] = lrq_detail::unpack(
            m_inner->head.load(std::memory_order_acquire));
        (void)r;
        assert(lrq_detail::queue_len(steal, tail) < LOCAL_QUEUE_CAPACITY);
        m_inner->buffer[tail & LOCAL_QUEUE_MASK] = std::move(task);
        m_inner->tail.store(tail + 1, std::memory_order_release);
    }

    /// Push `task`, spilling to `overflow` if the ring is full.
    ///
    /// Overflow must satisfy:
    ///   void push(T task)
    ///   void push_batch(T* tasks, std::size_t count)
    template<typename Overflow>
    void push_or_overflow(T task, Overflow& overflow) {
        uint32_t tail;
        while (true) {
            uint64_t head_val = m_inner->head.load(std::memory_order_acquire);
            auto [steal, real] = lrq_detail::unpack(head_val);
            tail = m_inner->tail.load(std::memory_order_relaxed);

            if (lrq_detail::queue_len(steal, tail) < LOCAL_QUEUE_CAPACITY) {
                break;   // room available
            }

            if (steal != real) {
                // Steal in progress — capacity will free up shortly.
                // Send directly to the injection queue rather than spinning.
                overflow.push(std::move(task));
                return;
            }

            if (push_overflow(std::move(task), real, tail, overflow))
                return;
            // CAS lost to a concurrent stealer — reload task and retry.
            // push_overflow moved task only on success; on failure task is intact.
        }
        push_back_finish(std::move(task), tail);
    }

    /// Pop the next task for the owner to run.
    /// Returns a default-constructed (falsy) T if the ring is empty.
    T pop() noexcept {
        uint64_t head_val = m_inner->head.load(std::memory_order_acquire);

        while (true) {
            auto [steal, real] = lrq_detail::unpack(head_val);
            uint32_t tail = m_inner->tail.load(std::memory_order_relaxed);

            if (real == tail) return nullptr;   // empty

            uint32_t next_real = real + 1;
            // If no steal is in progress, advance both steal and real together.
            // If a steal is in progress, advance only real so the stealer's
            // copy window is preserved.
            uint64_t next = (steal == real)
                ? lrq_detail::pack(next_real, next_real)
                : lrq_detail::pack(steal,     next_real);

            if (m_inner->head.compare_exchange_weak(
                    head_val, next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return std::move(m_inner->buffer[real & LOCAL_QUEUE_MASK]);
            }
        }
    }

private:
    void push_back_finish(T task, uint32_t tail) noexcept {
        m_inner->buffer[tail & LOCAL_QUEUE_MASK] = std::move(task);
        m_inner->tail.store(tail + 1, std::memory_order_release);
    }

    // Spill the second half of a full ring + `task` to `overflow`.
    // Returns true on success; false if the CAS failed (caller retries).
    //
    // After a successful spill the first half is re-exposed to stealers.
    // The second half is chosen so that tasks recycled from the injection queue
    // are not immediately spilled back to it on the very next push.
    template<typename Overflow>
    bool push_overflow(T task, uint32_t head, uint32_t tail, Overflow& overflow) {
        static constexpr uint32_t NUM_TAKEN =
            static_cast<uint32_t>(LOCAL_QUEUE_CAPACITY / 2);

        assert(lrq_detail::queue_len(head, tail) == LOCAL_QUEUE_CAPACITY);

        // Atomically claim the entire buffer: set both steal and real to `tail`
        // so stealers see the queue as empty while we move tasks out.
        uint64_t expected = lrq_detail::pack(head, head);
        uint64_t desired  = lrq_detail::pack(tail, tail);
        if (!m_inner->head.compare_exchange_weak(
                expected, desired,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return false;   // another thread raced; caller retries
        }

        // Re-expose the first half by advancing tail.  Because head.real = old
        // tail (= head + 256), the ring wraps and slots [head, head+128) map to
        // the same physical buffer positions as before — the first 128 tasks are
        // still there and are now visible to stealers again.
        m_inner->tail.store(tail + NUM_TAKEN, std::memory_order_release);

        // Move the second half into a stack batch and send to overflow.
        T batch[NUM_TAKEN + 1];
        uint32_t src = head + NUM_TAKEN;
        for (uint32_t i = 0; i < NUM_TAKEN; ++i)
            batch[i] = std::move(m_inner->buffer[(src + i) & LOCAL_QUEUE_MASK]);
        batch[NUM_TAKEN] = std::move(task);

        overflow.push_batch(batch, NUM_TAKEN + 1);
        return true;
    }

    std::shared_ptr<LocalRunQueueInner<T>> m_inner;
};

// ─── Steal ────────────────────────────────────────────────────────────────────

/// Consumer/thief handle.  Cloneable; may be used from any thread.
template<typename T>
class Steal {
public:
    explicit Steal(std::shared_ptr<LocalRunQueueInner<T>> inner) noexcept
        : m_inner(std::move(inner)) {}

    Steal(const Steal&)            = default;
    Steal& operator=(const Steal&) = default;
    Steal(Steal&&) noexcept            = default;
    Steal& operator=(Steal&&) noexcept = default;

    std::size_t len() const noexcept {
        auto [s, head] = lrq_detail::unpack(
            m_inner->head.load(std::memory_order_acquire));
        (void)s;
        uint32_t tail = m_inner->tail.load(std::memory_order_acquire);
        bool lifo = static_cast<bool>(
            m_inner->lifo.load(std::memory_order_relaxed));
        return lrq_detail::queue_len(head, tail) + (lifo ? 1u : 0u);
    }

    bool is_empty() const noexcept { return len() == 0; }

    /// Steal approximately half the tasks from self and place them into `dst`.
    /// Returns one task for the caller to run immediately, or null if nothing
    /// was available.
    ///
    /// Must only be called by `dst`'s owner thread when that thread is idle.
    T steal_into(Local<T>& dst) noexcept {
        uint32_t dst_tail = dst.m_inner->tail.load(std::memory_order_relaxed);

        // Abort if dst is already more than half full — not worth the trouble.
        auto [dst_steal, dr] = lrq_detail::unpack(
            dst.m_inner->head.load(std::memory_order_acquire));
        (void)dr;
        if (lrq_detail::queue_len(dst_steal, dst_tail) > LOCAL_QUEUE_CAPACITY / 2)
            return nullptr;

        uint32_t n = steal_into2(dst, dst_tail);

        if (n == 0) {
            // Ring empty — fall back to the LIFO slot.
            return m_inner->lifo.exchange(nullptr, std::memory_order_acq_rel);
        }

        --n;
        T ret = std::move(dst.m_inner->buffer[(dst_tail + n) & LOCAL_QUEUE_MASK]);

        if (n > 0)
            dst.m_inner->tail.store(dst_tail + n, std::memory_order_release);

        return ret;
    }

private:
    // Copy approximately half the tasks from self into dst's buffer.
    // Returns the number of tasks copied (may be 0 if nothing available or
    // another steal is already in progress).
    uint32_t steal_into2(Local<T>& dst, uint32_t dst_tail) noexcept {
        uint64_t prev = m_inner->head.load(std::memory_order_acquire);
        uint64_t next;
        uint32_t n;

        // Phase 1: claim ceiling-half of available tasks by advancing real.
        while (true) {
            auto [src_steal, src_real] = lrq_detail::unpack(prev);
            uint32_t src_tail = m_inner->tail.load(std::memory_order_acquire);

            // Another steal is in progress — back off to avoid aliased copies.
            if (src_steal != src_real) return 0;

            // Number to steal: ceiling half of available tasks.
            uint32_t avail = src_tail - src_real;   // wrapping subtraction
            n = avail - avail / 2;

            if (n == 0) return 0;

            // Claim by advancing real (not steal) — marks steal as in-progress.
            // No other stealer can proceed until steal catches up to real again.
            uint32_t steal_to = src_real + n;
            assert(src_steal != steal_to);
            next = lrq_detail::pack(src_steal, steal_to);

            if (m_inner->head.compare_exchange_weak(
                    prev, next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
            // prev updated by CAS failure — retry.
        }

        assert(n <= LOCAL_QUEUE_CAPACITY / 2);

        // Phase 2: move claimed tasks into dst's buffer.
        // `first` is src_steal (== src_real from before the CAS) — the start of
        // the claimed range.
        uint32_t first = lrq_detail::unpack(next).first;
        for (uint32_t i = 0; i < n; ++i) {
            dst.m_inner->buffer[(dst_tail + i) & LOCAL_QUEUE_MASK] =
                std::move(m_inner->buffer[(first + i) & LOCAL_QUEUE_MASK]);
        }

        // Phase 3: clear in-progress marker by advancing steal to match real.
        // real may have advanced since our CAS if the owner called pop() during
        // the copy; we accept that and just set steal = real.
        while (true) {
            uint32_t real = lrq_detail::unpack(prev).second;
            next = lrq_detail::pack(real, real);

            if (m_inner->head.compare_exchange_weak(
                    prev, next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return n;
            }
            // prev updated — retry with the new real value.
        }
    }

    std::shared_ptr<LocalRunQueueInner<T>> m_inner;
};

// ─── factory ─────────────────────────────────────────────────────────────────

/// Create a linked (Steal<T>, Local<T>) pair backed by the same inner state.
template<typename T>
std::pair<Steal<T>, Local<T>> make_local_run_queue() {
    auto inner = std::make_shared<LocalRunQueueInner<T>>();
    return { Steal<T>(inner), Local<T>(inner) };
}

} // namespace coro::detail
