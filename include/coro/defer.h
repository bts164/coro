#pragma once

#include <concepts>
#include <stdexcept>
#include <utility>

namespace coro {

/**
 * @brief Calls a function when it goes out of scope, however the scope ends, like Go's
 * `defer`.
 *
 * The RAII handle for a cleanup that has no type of its own: undoing a count, restoring a
 * setting, marking a state. In a coroutine this matters more than in ordinary code. A
 * cancelled coroutine is never resumed: its frame is destroyed, and only destructors run,
 * so a `catch` block or a cleanup line after the last `co_await` is skipped. A `Defer`'s
 * destructor still runs (guideline CA.1).
 *
 * ```cpp
 * coro::Coro<void> watch(coro::WatchSender<Settings> settings) {
 *     settings.send_if_modified([](Settings& s) { ++s.viewers; return true; });
 *     coro::Defer uncount([&settings] {
 *         settings.send_if_modified([](Settings& s) { --s.viewers; return true; });
 *     });
 *     co_await serve_viewer();  // a cancel here still runs `uncount`
 * }
 * ```
 *
 * **Name it.** An unnamed `Defer` is a temporary: it is destroyed at the end of its
 * statement, so the function runs at once instead of at the end of the scope. This is the
 * same mistake as an unnamed `std::lock_guard`. The constructor is `[[nodiscard]]`, so
 * GCC and Clang warn about it.
 *
 * ```cpp
 * coro::Defer([&] { --count; });        // WRONG: runs here, at once
 * coro::Defer uncount([&] { --count; }); // runs when the scope ends
 * ```
 *
 * **Capturing by reference is safe:** the `Defer` lives in the same scope as what it
 * captures, and locals declared before it are destroyed after it. This is not the
 * capturing-lambda-coroutine hazard; the function is an ordinary lambda, not a coroutine.
 *
 * **Operations.** A `Defer` is armed when it is constructed. Exactly one of these disarms
 * it:
 *
 * - The end of its scope: the destructor calls the function.
 * - `operator()`: calls the function now, so the end of the scope does nothing.
 * - `dismiss()`: disarms it without calling the function, so it is never called.
 *
 * So the function is called at most once. Calling `operator()` or `dismiss()` on a
 * disarmed `Defer` throws `std::logic_error`. A `Defer` can be neither copied nor moved,
 * so no other object can call the function either.
 *
 * Every exit after `operator()` or `dismiss()`, a cancel included, skips the function.
 * Call either only once no later `co_await` needs the function to cover it.
 *
 * **Example: releasing early.** A path that is done with something sooner releases it
 * sooner, while every other path still releases it when the scope ends:
 *
 * ```cpp
 * coro::Defer uncount([&] { --count; });
 * if (finished_early) uncount();  // called here; the end of the scope does nothing
 * co_await more_work();           // otherwise called when the scope ends
 * ```
 *
 * **Example: rollback.** Undo a change made in several steps, unless every step succeeds:
 *
 * ```cpp
 * registry.add(id);
 * coro::Defer remove([&] { registry.remove(id); });
 * co_await send_welcome();  // a throw or a cancel here removes `id` again
 * remove.dismiss();         // every step succeeded: keep it
 * ```
 *
 * **The function:**
 *
 * - Cannot `co_await`: it is an ordinary function, called from the destructor or from
 *   `operator()`.
 * - Must not throw when the destructor runs it: an exception escaping a destructor calls
 *   `std::terminate()`. An early call is ordinary code, so an exception from it reaches
 *   the caller. The function counts as run all the same, and the destructor does not try
 *   it again.
 * - Runs, when the scope ends, on whichever thread ends it. When a cancel destroys a
 *   coroutine's frame, that may not be the thread the coroutine last ran on, so it should
 *   not depend on which thread it is.
 *
 * @tparam F A callable taking no arguments. Its result, if any, is discarded.
 */
template<std::invocable F>
class Defer {
public:
    /// @brief Stores `fn`, to be called when this `Defer` is destroyed.
    // [[nodiscard]] on the constructor, not the class: only that makes GCC warn about an
    // unnamed Defer.
    [[nodiscard]] explicit Defer(F fn) : m_fn(std::move(fn)) {}

    /// @brief Calls the stored function, unless `operator()` or `dismiss()` disarmed it.
    ~Defer() {
        if (m_armed) m_fn();
    }

    /**
     * @brief Calls the stored function now, so that the destructor doesn't.
     *
     * Disarms the `Defer` before calling: if the function throws, the exception reaches the
     * caller, and the destructor still doesn't call it again.
     *
     * @throws std::logic_error if `operator()` or `dismiss()` has already been called.
     */
    void operator()() {
        disarm();
        m_fn();
    }

    /**
     * @brief Disarms the `Defer` without calling the stored function, which is then never
     * called.
     *
     * @throws std::logic_error if `operator()` or `dismiss()` has already been called.
     */
    void dismiss() { disarm(); }

    Defer(const Defer&) = delete;
    Defer& operator=(const Defer&) = delete;

private:
    void disarm() {
        if (!m_armed) throw std::logic_error("coro::Defer already called or dismissed");
        // RACE: m_armed is unsynchronised: a Defer is a local with one owner, so two
        // threads using the same object is already misuse.
        m_armed = false;
    }

    F m_fn;
    bool m_armed = true;
};

}  // namespace coro
