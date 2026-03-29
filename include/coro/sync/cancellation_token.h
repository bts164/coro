#pragma once

namespace coro {

// Placeholder for the cancellation system, to be designed in a future phase.
// Context carries a shared_ptr<CancellationToken> from the start so that
// adding cancellation later does not require changing every poll() signature.
class CancellationToken {
public:
    virtual ~CancellationToken();
};

} // namespace coro
