#include <coro/detail/context.h>

namespace coro::detail {

Context::Context(Rc<Waker> waker)
    : m_waker(std::move(waker)) {}

Context::~Context() = default;

Rc<Waker> Context::getWaker() const {
    return m_waker;
}

Weak<Waker> Context::get_weak_waker() const {
    return m_waker;
}

} // namespace coro::detail
