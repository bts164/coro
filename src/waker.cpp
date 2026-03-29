#include <coro/waker.h>

namespace coro {

// Out-of-line destructor anchors the vtable to this translation unit.
Waker::~Waker() = default;

} // namespace coro
