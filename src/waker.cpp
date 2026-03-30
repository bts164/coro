#include <coro/detail/waker.h>

namespace coro::detail {

// Out-of-line destructor anchors the vtable to this translation unit.
Waker::~Waker() = default;

} // namespace coro
