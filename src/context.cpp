#include <coro/context.h>

namespace coro {

Context::Context(std::shared_ptr<Waker> waker,
                 std::shared_ptr<CancellationToken> token)
    : m_waker(std::move(waker))
    , m_token(std::move(token)) {}

Context::~Context() = default;

std::shared_ptr<Waker> Context::getWaker() const {
    return m_waker;
}

std::shared_ptr<CancellationToken> Context::getCancellationToken() const {
    return m_token;
}

} // namespace coro
