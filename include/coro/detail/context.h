#pragma once

#include <coro/cancellation_token.h>
#include <coro/waker.h>
#include <memory>

namespace coro {

// Passed to poll() / poll_next() on each scheduling tick.
// Concrete executor implementations may subclass Context to carry
// additional scheduler-specific state.
class Context {
public:
    explicit Context(std::shared_ptr<Waker> waker,
                     std::shared_ptr<CancellationToken> token = nullptr);
    virtual ~Context();

    std::shared_ptr<Waker>             getWaker() const;
    std::shared_ptr<CancellationToken> getCancellationToken() const;

private:
    std::shared_ptr<Waker>             m_waker;
    std::shared_ptr<CancellationToken> m_token;
};

} // namespace coro
