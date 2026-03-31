#pragma once

#include <utility>

namespace coro {

/**
 * @brief Transport-layer error codes returned by channel operations.
 *
 * These codes describe infrastructure failures — whether the channel is open
 * and whether the other end is alive. Application-level errors should be sent
 * as values; see the "Sending errors through a channel" section in doc/channels.md.
 */
enum class ChannelError {
    Closed,           ///< Sender dropped without sending (oneshot only).
    SenderDropped,    ///< All senders were dropped; no more values will arrive (mpsc, watch).
    ReceiverDropped,  ///< Receiver was dropped; sent value is returned to caller (mpsc).
    Empty,            ///< tryRecv only — channel is open but no value is available yet.
};

/**
 * @brief Error type returned by `trySend` on failure.
 *
 * Carries both the reason the send failed and the unsent value so move-only
 * types can be recovered by the caller.
 *
 * @tparam T The value type that could not be sent.
 */
template<typename T>
struct TrySendError {
    enum class Kind {
        Full,         ///< Buffer is full; the caller may retry later.
        Disconnected, ///< Receiver was dropped; retrying will never succeed.
    } kind;

    T value; ///< The unsent value, always present on failure.

    TrySendError(Kind k, T v) : kind(k), value(std::move(v)) {}
};

} // namespace coro
