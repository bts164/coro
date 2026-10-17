#include <coro/detail/context.h>

namespace coro::detail {

Context::Context(Rc<Waker> waker, std::uint32_t budget)
    : m_owner(std::move(waker)), m_waker(m_owner.get()), m_budget(budget) {}

Context::Context(Waker& waker, std::uint32_t budget)
    : m_waker(&waker), m_budget(budget) {}

Context::~Context() = default;

void Context::yield_to_executor() {
    m_yielded = true;
    // m_budget is left as it is: the rest of this task poll (the other branches of a
    // select() or join() above the caller) may still use it. Setting it to 0 here
    // would make every later co_await in this poll yield too, ending the poll
    // promptly. See the note under "yield_now()" in doc/design/task_budget.md.
    // The task is being polled, so this is Running → RunningAndNotified and the
    // executor re-queues it when poll() returns. A wake from another thread that
    // lands at the same moment finds the state already changed and does nothing.
    m_waker->wake();
}

Rc<Waker> Context::getWaker() const {
    if (m_owner || !m_waker) return m_owner;
    return m_waker->clone();
}

Weak<Waker> Context::get_weak_waker() const {
    if (m_owner || !m_waker) return m_owner;
    return m_waker->weak_clone();
}

} // namespace coro::detail
