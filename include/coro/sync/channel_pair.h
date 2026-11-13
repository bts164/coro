#pragma once

namespace coro {

/**
 * @brief The two ends of a channel, as every channel factory returns them.
 *
 * `first` is the sending end and `second` the receiving end, as in a `std::pair`, so
 * `auto [tx, rx] = mpsc_channel<int>(8);` and `.first` / `.second` read the same as before.
 *
 * Deliberately a plain aggregate, not a `std::pair`, and deliberately without the tuple
 * protocol (`std::tuple_size` / `get<I>`): GCC 15 at -O1 and above never runs the destructor
 * of a structured binding's hidden object when the binding goes through the tuple protocol and
 * the object lives in a coroutine frame across a suspension. Bound that way, a channel's ends
 * leaked their shared state. A plain aggregate is bound member by member, which GCC gets
 * right. See doc/guidelines.md, GCC.1, and test/compiler/structured_binding_leak.cpp.
 * Don't add a tuple protocol to this type: test_channel_pair.cpp fails if it gains one.
 *
 * @tparam Sender   The sending end's type.
 * @tparam Receiver The receiving end's type.
 */
template<typename Sender, typename Receiver>
struct ChannelPair {
    Sender   first;
    Receiver second;
};

} // namespace coro
