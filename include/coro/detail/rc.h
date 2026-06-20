#pragma once

// Platform-portable reference-counted pointer types.
//
//   Multi-threaded platforms (non-CORO_PICO):
//     Task ownership and waker plumbing are shared across threads (a
//     spawn_blocking worker, a libuv I/O callback, or any external thread
//     that receives a waker), so the standard atomic-refcount std::shared_ptr
//     / std::weak_ptr is required.
//
//   CORO_PICO (RP2040 bare-metal):
//     CurrentThreadExecutor is cooperative and single-threaded. co_await is
//     the only yield point, so no two coroutines ever interleave, and these
//     pointers are never touched from an ISR (see isr_safety.md) or a second
//     core. See doc/design/pico_port.md's "Non-atomic refcounting and
//     scheduling state" section for the rationale: the RP2040's Cortex-M0+
//     has no LDREX/STREX, so
//     std::shared_ptr's atomic refcounting falls back to a software critical
//     section (masking IRQs) on every copy/destroy — overhead that buys
//     nothing on a target that is provably single-threaded. Rc<T>/Weak<T>
//     replace it with a plain non-atomic intrusive-refcount pointer that
//     mirrors the subset of std::shared_ptr/std::weak_ptr's API actually
//     used by the library: make_rc, copy/move, the aliasing constructor,
//     implicit upcast across multiple inheritance, and Weak<T>::lock().
//
// Usage:
//   #include <coro/detail/rc.h>
//   coro::detail::Rc<Foo> p = coro::detail::make_rc<Foo>(args...);
//   coro::detail::Weak<Foo> w = p;

#ifdef CORO_PICO

#include <cstddef>
#include <memory>  // for std::hash
#include <type_traits>
#include <utility>

namespace coro::detail {

// Base of every control block, type-erased so Weak<T> can outlive the value
// without needing to know T. Plain ints, not atomics — see the file comment.
class RcControlBlockBase {
public:
    virtual ~RcControlBlockBase() = default;

    void add_strong_ref() { ++strong_count; }
    void add_weak_ref()   { ++weak_count; }

    // Returns true if the control block itself should now be freed (both
    // counts have reached zero). The value is destroyed as soon as
    // strong_count reaches zero, but the block itself must outlive the value
    // while weak references still exist (Weak::lock() must be able to
    // observe strong_count == 0 rather than touching freed memory) — so the
    // block is only freed once neither a strong nor a weak holder remains.
    //
    // m_destroying guards against a self-referential weak pointer (e.g.
    // TaskBase::m_self, used for shared_from_this()) freeing the block from
    // *inside* destroy_value(): if a weak ref dropped to zero during
    // destroy_value() triggered `delete` immediately, that delete would free
    // the very storage the in-progress destructor (T::~T(), still running as
    // part of this same call) lives in — a use-after-free the instant the
    // destructor's remaining code touches `this`. release_weak() defers the
    // free decision back to release_strong(), which re-checks weak_count only
    // after destroy_value() has fully returned.
    bool release_strong() {
        if (--strong_count == 0) {
            m_destroying = true;
            destroy_value();
            m_destroying = false;
            return weak_count == 0;
        }
        return false;
    }

    // Returns true if the control block itself should now be freed.
    bool release_weak() {
        --weak_count;
        if (m_destroying) return false;  // see release_strong()
        return weak_count == 0 && strong_count == 0;
    }

    int strong_count = 1;
    int weak_count   = 0;

private:
    bool m_destroying = false;

protected:
    virtual void destroy_value() = 0;
};

// Holds T inline (constructed via make_rc) so a single allocation backs both
// the control block and the value, mirroring std::make_shared.
template<typename T>
class RcControlBlock final : public RcControlBlockBase {
public:
    template<typename... Args>
    explicit RcControlBlock(Args&&... args) {
        ::new (static_cast<void*>(&storage)) T(std::forward<Args>(args)...);
    }

    T* get() { return reinterpret_cast<T*>(&storage); }

protected:
    void destroy_value() override { get()->~T(); }

private:
    alignas(T) std::byte storage[sizeof(T)];
};

template<typename T> class NonAtomicWeak;

template<typename T>
class NonAtomicRc {
public:
    NonAtomicRc() = default;
    NonAtomicRc(std::nullptr_t) {}

    NonAtomicRc(const NonAtomicRc& other) : m_ptr(other.m_ptr), m_block(other.m_block) {
        if (m_block) m_block->add_strong_ref();
    }

    NonAtomicRc(NonAtomicRc&& other) noexcept
        : m_ptr(other.m_ptr), m_block(other.m_block) {
        other.m_ptr = nullptr;
        other.m_block = nullptr;
    }

    // Implicit upcast across (possibly multiple) inheritance — U* must be
    // convertible to T*; the compiler performs any necessary pointer
    // adjustment when U* is converted to T* below.
    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    NonAtomicRc(const NonAtomicRc<U>& other) : m_ptr(other.m_ptr), m_block(other.m_block) {
        if (m_block) m_block->add_strong_ref();
    }

    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    NonAtomicRc(NonAtomicRc<U>&& other) noexcept : m_ptr(other.m_ptr), m_block(other.m_block) {
        other.m_ptr = nullptr;
        other.m_block = nullptr;
    }

    // Aliasing constructor: shares ownership (and the control block) with
    // `owner`, but this->get() returns `ptr` — used when a JoinHandle<T>/
    // StreamHandle<T> wants to view a TaskState<T> that's a base of the same
    // allocation as the owning TaskImpl<F>.
    template<typename U>
    NonAtomicRc(const NonAtomicRc<U>& owner, T* ptr) : m_ptr(ptr), m_block(owner.m_block) {
        if (m_block) m_block->add_strong_ref();
    }

    ~NonAtomicRc() { release(); }

    NonAtomicRc& operator=(const NonAtomicRc& other) {
        if (this != &other) {
            release();
            m_ptr = other.m_ptr;
            m_block = other.m_block;
            if (m_block) m_block->add_strong_ref();
        }
        return *this;
    }

    NonAtomicRc& operator=(NonAtomicRc&& other) noexcept {
        if (this != &other) {
            release();
            m_ptr = other.m_ptr;
            m_block = other.m_block;
            other.m_ptr = nullptr;
            other.m_block = nullptr;
        }
        return *this;
    }

    NonAtomicRc& operator=(std::nullptr_t) {
        release();
        m_ptr = nullptr;
        m_block = nullptr;
        return *this;
    }

    void reset() {
        release();
        m_ptr = nullptr;
        m_block = nullptr;
    }

    T* get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    T& operator*() const { return *m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    friend bool operator==(const NonAtomicRc& a, const NonAtomicRc& b) { return a.m_ptr == b.m_ptr; }
    friend bool operator==(const NonAtomicRc& a, std::nullptr_t) { return a.m_ptr == nullptr; }
    friend bool operator!=(const NonAtomicRc& a, const NonAtomicRc& b) { return a.m_ptr != b.m_ptr; }
    friend bool operator!=(const NonAtomicRc& a, std::nullptr_t) { return a.m_ptr != nullptr; }

private:
    void release() {
        // release_strong() already reports whether weak_count == 0 (i.e.
        // whether the block should be freed) — do not call release_weak()
        // here too. That second call would decrement weak_count without a
        // matching add_weak_ref(), permanently leaking the block whenever a
        // strong release is the one that brings both counts to zero.
        if (m_block && m_block->release_strong()) {
            delete m_block;
        }
    }

    T* m_ptr = nullptr;
    RcControlBlockBase* m_block = nullptr;

    template<typename U> friend class NonAtomicRc;
    template<typename U> friend class NonAtomicWeak;
    template<typename U, typename... Args> friend NonAtomicRc<U> make_rc(Args&&...);
};

template<typename T>
class NonAtomicWeak {
public:
    NonAtomicWeak() = default;

    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    NonAtomicWeak(const NonAtomicRc<U>& rc) : m_ptr(rc.m_ptr), m_block(rc.m_block) {
        if (m_block) m_block->add_weak_ref();
    }

    NonAtomicWeak(const NonAtomicWeak& other) : m_ptr(other.m_ptr), m_block(other.m_block) {
        if (m_block) m_block->add_weak_ref();
    }

    NonAtomicWeak(NonAtomicWeak&& other) noexcept : m_ptr(other.m_ptr), m_block(other.m_block) {
        other.m_ptr = nullptr;
        other.m_block = nullptr;
    }

    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    NonAtomicWeak(const NonAtomicWeak<U>& other) : m_ptr(other.m_ptr), m_block(other.m_block) {
        if (m_block) m_block->add_weak_ref();
    }

    ~NonAtomicWeak() { release(); }

    NonAtomicWeak& operator=(const NonAtomicWeak& other) {
        if (this != &other) {
            release();
            m_ptr = other.m_ptr;
            m_block = other.m_block;
            if (m_block) m_block->add_weak_ref();
        }
        return *this;
    }

    NonAtomicWeak& operator=(NonAtomicWeak&& other) noexcept {
        if (this != &other) {
            release();
            m_ptr = other.m_ptr;
            m_block = other.m_block;
            other.m_ptr = nullptr;
            other.m_block = nullptr;
        }
        return *this;
    }

    // Templated on U (rather than fixed to T) so that overload resolution must perform
    // template argument deduction to select this candidate. This mirrors std::weak_ptr's
    // operator=(const shared_ptr<Y>&), which is also a deduced template — deduction fails
    // for a braced-init-list argument (e.g. `weak = {}`), so that overload silently drops
    // out of the candidate set, leaving only operator=(const NonAtomicWeak&). If this were
    // a fixed (non-template) overload for T, it would tie with operator=(const NonAtomicWeak&)
    // and make `weak = {}` ambiguous.
    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    NonAtomicWeak& operator=(const NonAtomicRc<U>& rc) {
        release();
        m_ptr = rc.m_ptr;
        m_block = rc.m_block;
        if (m_block) m_block->add_weak_ref();
        return *this;
    }

    NonAtomicRc<T> lock() const {
        NonAtomicRc<T> result;
        if (m_block && m_block->strong_count > 0) {
            m_block->add_strong_ref();
            result.m_ptr = m_ptr;
            result.m_block = m_block;
        }
        return result;
    }

    void reset() {
        release();
        m_ptr = nullptr;
        m_block = nullptr;
    }

    bool expired() const { return !m_block || m_block->strong_count == 0; }

private:
    void release() {
        if (m_block && m_block->release_weak()) delete m_block;
    }

    T* m_ptr = nullptr;
    RcControlBlockBase* m_block = nullptr;

    template<typename U> friend class NonAtomicWeak;
};

template<typename T, typename... Args>
NonAtomicRc<T> make_rc(Args&&... args) {
    auto* block = new RcControlBlock<T>(std::forward<Args>(args)...);
    NonAtomicRc<T> result;
    result.m_ptr = block->get();
    result.m_block = block;
    return result;
}

template<typename T> using Rc   = NonAtomicRc<T>;
template<typename T> using Weak = NonAtomicWeak<T>;

} // namespace coro::detail

namespace std {
// Required so CurrentThreadExecutor::m_owned_tasks (an
// unordered_set<coro::detail::Rc<TaskBase>>) can hash by pointer identity,
// matching std::shared_ptr's hash semantics.
template<typename T>
struct hash<coro::detail::NonAtomicRc<T>> {
    std::size_t operator()(const coro::detail::NonAtomicRc<T>& p) const noexcept {
        return std::hash<T*>{}(p.get());
    }
};
} // namespace std

#else  // ---- multi-threaded targets, std::shared_ptr/weak_ptr -------------

#include <memory>

namespace coro::detail {
    template<typename T> using Rc   = std::shared_ptr<T>;
    template<typename T> using Weak = std::weak_ptr<T>;

    template<typename T, typename... Args>
    Rc<T> make_rc(Args&&... args) {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
} // namespace coro::detail

#endif // CORO_PICO
