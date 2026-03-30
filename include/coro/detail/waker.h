#pragma once

#include <memory>

namespace coro::detail {

class Waker {
public:
    virtual ~Waker();
    virtual void wake() = 0;
    virtual std::shared_ptr<Waker> clone() = 0;
};

} // namespace coro
