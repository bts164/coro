#include <coro/detail/context.h>

namespace coro::detail {

Context::Context(std::shared_ptr<Waker> waker)
    : m_waker(std::move(waker)) {}

Context::~Context() = default;

std::shared_ptr<Waker> Context::getWaker() const {
    return m_waker;
}

std::weak_ptr<Waker> Context::get_weak_waker() const {
    return m_waker;
}

} // namespace coro::detail
