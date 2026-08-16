#include <coro/detail/fiber_context.h>

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <new>
#include <stdexcept>

// ASan doesn't understand a bare swapcontext() jumping between independently
// allocated stacks -- without the annotations below it treats the fiber's
// stack as unrelated memory and reports false-positive stack-buffer-overflow/
// use-after-return errors (and prints its own "doesn't fully support
// makecontext/swapcontext" warning even when nothing is actually wrong). The
// __sanitizer_{start,finish}_switch_fiber() pair tells it exactly what stack
// execution is jumping to/from so it can track redzones correctly instead of
// guessing. See https://github.com/google/sanitizers/issues/189.
#if defined(__SANITIZE_ADDRESS__)
#define CORO_FIBER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CORO_FIBER_ASAN 1
#endif
#endif

#if defined(CORO_FIBER_ASAN)
#include <sanitizer/common_interface_defs.h>
#endif

namespace coro::detail {

namespace {

#if defined(CORO_FIBER_ASAN)
// Carries the fake-stack handle from a switch_context() call's
// __sanitizer_start_switch_fiber() across to whichever code runs the matching
// __sanitizer_finish_switch_fiber(). For a resumed context that's exactly the
// same switch_context() call (swapcontext() just returns into it), but the
// *first* jump into a freshly makecontext()'d fiber lands in
// FiberFuture::trampoline() instead -- a different call entirely, reached
// without swapcontext() ever returning here -- so a thread_local is needed to
// carry the handle across that asymmetry. Safe as a thread_local rather than
// a per-FiberContext field: only one switch is ever in flight on a given
// thread at a time.
thread_local void* t_asan_fake_stack_save = nullptr;
#endif

} // namespace

void notify_fiber_switch_finished() {
#if defined(CORO_FIBER_ASAN)
    __sanitizer_finish_switch_fiber(t_asan_fake_stack_save, nullptr, nullptr);
#endif
}

namespace {

size_t page_size() {
    static const size_t sz = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return sz;
}

size_t round_up_to_page(size_t n) {
    const size_t page = page_size();
    return (n + page - 1) / page * page;
}

} // namespace

void alloc_fiber_stack(FiberContext& fc, size_t stack_size) {
    const size_t page  = page_size();
    const size_t usable = round_up_to_page(stack_size);
    const size_t total  = usable + page; // one guard page below the usable region

    void* mem = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        throw std::bad_alloc();

    if (mprotect(mem, page, PROT_NONE) != 0) {
        munmap(mem, total);
        throw std::runtime_error("mprotect failed setting up fiber stack guard page");
    }

    fc.mmap_base  = mem;
    fc.mmap_size  = total;
    fc.stack_base = static_cast<std::byte*>(mem) + page;
    fc.stack_size = usable;
}

void free_fiber_stack(FiberContext& fc) noexcept {
    if (fc.mmap_base) {
        munmap(fc.mmap_base, fc.mmap_size);
        fc.mmap_base = nullptr;
    }
}

FiberContext::~FiberContext() { free_fiber_stack(*this); }

void init_fiber_entry(FiberContext& fc, void (*entry)()) {
    getcontext(&fc.uc);
    fc.uc.uc_stack.ss_sp   = fc.stack_base;
    fc.uc.uc_stack.ss_size = fc.stack_size;
    fc.uc.uc_link          = nullptr; // trampoline switches back explicitly; see FiberFuture::trampoline()
    makecontext(&fc.uc, entry, 0);
}

void switch_context(FiberContext* from, FiberContext* to) {
#if defined(CORO_FIBER_ASAN)
    // `to->stack_base`/`stack_size` are null/0 for a caller-side FiberContext
    // with no dedicated stack (see FiberContext's comment) -- passing that
    // through is exactly the documented way to tell ASan "switching back to
    // the stack this thread was already running on", not a separate fiber
    // stack.
    __sanitizer_start_switch_fiber(&t_asan_fake_stack_save, to->stack_base, to->stack_size);
#endif

    swapcontext(&from->uc, &to->uc);

    // Only reached when swapcontext() above returns into THIS call, i.e. when
    // resuming a context that was itself suspended mid-switch_context(). The
    // first-ever jump into a fresh fiber instead lands in
    // FiberFuture::trampoline(), which calls notify_fiber_switch_finished()
    // itself for that case -- see its comment.
#if defined(CORO_FIBER_ASAN)
    notify_fiber_switch_finished();
#endif
}

} // namespace coro::detail
