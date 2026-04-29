#include <coro/detail/context.h>

namespace coro::detail {

Context::Context(std::shared_ptr<Waker> waker,
                 std::shared_ptr<CancellationToken> token)
    : m_waker(std::move(waker))
    , m_token(std::move(token)) {}

Context::~Context() = default;

std::shared_ptr<Waker> Context::getWaker() const {
    return m_waker;
}

std::weak_ptr<Waker> Context::get_weak_waker() const {
    return m_waker;
}

std::shared_ptr<CancellationToken> Context::getCancellationToken() const {
    return m_token;
}

} // namespace coro::detail
