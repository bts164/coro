#pragma once

// Fixed-size-class freelist allocator, intended as a faster operator new/delete
// backing for coroutine frames on CORO_PICO (see coro.h's CoroPromiseBase).
// Not itself gated by CORO_PICO — the data structure is platform-agnostic and
// unit-tested on desktop; only its use as a promise-type allocator is Pico-only.
//
// Single-threaded only: no locking. Safe under CurrentThreadExecutor's
// cooperative model for the same reason Mutex is a no-op there (see mutex.h).

#include <cstddef>
#include <cstdint>
#include <new>

namespace coro::detail {

// SlotSize must be >= sizeof(void*) — free slots store the freelist link in
// their own storage (intrusive freelist), so a slot must be at least pointer-sized.
template<std::size_t SlotSize, std::size_t SlotCount>
class FramePool {
public:
    static_assert(SlotSize >= sizeof(void*));

    FramePool() noexcept {
        for (std::size_t i = 0; i + 1 < SlotCount; ++i)
            next_of(slot(i)) = slot(i + 1);
        if constexpr (SlotCount > 0)
            next_of(slot(SlotCount - 1)) = nullptr;
        m_free_list = SlotCount > 0 ? slot(0) : nullptr;
    }

    FramePool(const FramePool&)            = delete;
    FramePool& operator=(const FramePool&) = delete;

    // Returns nullptr if the pool is exhausted — caller falls back to ::operator new.
    void* allocate() noexcept {
        if (!m_free_list) return nullptr;
        void* p = m_free_list;
        m_free_list = next_of(p);
        return p;
    }

    void deallocate(void* p) noexcept {
        next_of(p) = m_free_list;
        m_free_list = p;
    }

    // Used by operator delete to route a pointer back to the pool it came from
    // (rather than trusting the deallocation size, which would misroute a
    // pointer that fell through to ::operator new when this pool was exhausted).
    bool owns(const void* p) const noexcept {
        auto addr = reinterpret_cast<std::uintptr_t>(p);
        auto base = reinterpret_cast<std::uintptr_t>(m_storage);
        return addr >= base && addr < base + sizeof(m_storage);
    }

    static constexpr std::size_t slot_size = SlotSize;

private:
    void*& next_of(void* p) noexcept { return *reinterpret_cast<void**>(p); }
    void* slot(std::size_t i) noexcept { return &m_storage[i * SlotSize]; }

    alignas(std::max_align_t) std::byte m_storage[SlotSize * SlotCount];
    void* m_free_list;
};

} // namespace coro::detail
