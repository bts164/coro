#pragma once

namespace coro {

/**
 * @brief Placeholder for the cooperative cancellation system.
 *
 * `CancellationToken` is reserved for a future design phase. A `shared_ptr<CancellationToken>`
 * is already threaded through @ref Context so that adding cancellation support later does
 * not require changing every `poll()` signature.
 */
class CancellationToken {
public:
    virtual ~CancellationToken();
};

} // namespace coro
