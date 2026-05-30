#pragma once

#include <coro/coro_stream.h>
#include <coro/io/byte_source.h>
#include <coroutine>
#include <type_traits>

namespace coro {

template<typename T>
class DecoderStream;

// ---------------------------------------------------------------------------
// DecoderCoroPromise<T>
// ---------------------------------------------------------------------------

/**
 * @brief Promise type for DecoderStream<T>.
 *
 * Inherits all of CoroStream<T>::promise_type except:
 * - `get_return_object()` — returns `DecoderStream<T>` instead of `CoroStream<T>`.
 * - `await_transform()` — restricted to @ref ByteSourceFuture types only.
 *   Any `co_await` of a non-ByteSource future is a hard compile error.
 *
 * Layout note: `DecoderCoroPromise<T>` uses single, non-virtual inheritance
 * from `CoroStream<T>::promise_type`, so the base subobject shares the same
 * address as the derived object. `get_return_object()` exploits this to call
 * `coroutine_handle<base_promise>::from_promise(static_cast<base&>(*this))`,
 * which correctly identifies this coroutine's frame.
 */
template<typename T>
struct DecoderCoroPromise : CoroStream<T>::promise_type {
    using base_promise = typename CoroStream<T>::promise_type;

    DecoderStream<T> get_return_object() {
        auto base_handle =
            std::coroutine_handle<base_promise>::from_promise(
                static_cast<base_promise&>(*this));

        return DecoderStream<T>(base_handle);
    }

    // Forward ByteSource futures to the base await_transform machinery.
    template<ByteSourceFuture F>
    auto await_transform(F&& f) {
        return base_promise::await_transform(std::forward<F>(f));
    }

    // Hard compile error for any non-ByteSource co_await expression.
    template<typename F> requires (!ByteSourceFuture<std::remove_cvref_t<F>>)
    void await_transform(F&&) = delete;  // Decoder may only co_await ByteSource futures
};

// ---------------------------------------------------------------------------
// DecoderStream<T>
// ---------------------------------------------------------------------------

/**
 * @brief Coroutine return type for PollStream decoders.
 *
 * Identical to `CoroStream<T>` but its promise type (@ref DecoderCoroPromise)
 * restricts `co_await` to `ByteSource`-produced futures only. Any attempt to
 * `co_await` a non-ByteSource future inside a `DecoderStream` coroutine is a
 * hard compile error.
 *
 * `DecoderStream<T>` publicly inherits `CoroStream<T>` with no added data
 * members, so it can be move-constructed into a `CoroStream<T>` wherever the
 * infrastructure expects one.
 *
 * Usage:
 * @code
 * coro::DecoderStream<PciePacket> pcie_decoder(coro::ByteSource& src) {
 *     for (;;) {
 *         PciePacket pkt;
 *         if (co_await src.memcpy(&pkt.header1, sizeof(pkt.header1)) == 0)
 *             co_return; // clean EOF between packets
 *         // ... decode rest of packet ...
 *         co_yield std::move(pkt);
 *     }
 * }
 * auto stream = coro::PollStream<PciePacket>::open(fd, pcie_decoder);
 * @endcode
 *
 * @tparam T The item type yielded by the decoder.
 */
template<typename T>
class DecoderStream : public CoroStream<T> {
public:
    using promise_type = DecoderCoroPromise<T>;
    using CoroStream<T>::CoroStream;

    DecoderStream(DecoderStream&&) noexcept = default;
    DecoderStream& operator=(DecoderStream&&) noexcept = default;
};

} // namespace coro
