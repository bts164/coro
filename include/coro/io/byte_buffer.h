#pragma once

#include <concepts>
#include <cstddef>
#include <ranges>

namespace coro {

/**
 * @brief Concept for a movable, contiguous, byte-sized buffer type.
 *
 * Satisfied by `std::vector<std::byte>`, `std::array<std::byte, N>`, `std::string`,
 * `std::vector<char>`, and any contiguous range whose element type is exactly one byte.
 *
 * Async I/O methods (`TcpStream::read`, `TcpStream::write`) accept any `ByteBuffer` by
 * value so that ownership is transferred into the operation — eliminating the class of
 * bugs where a caller's buffer is freed before the I/O callback fires.
 */
template <typename T>
concept ByteBuffer =
    std::movable<T> &&
    std::ranges::contiguous_range<T> &&
    sizeof(std::ranges::range_value_t<T>) == 1;

/**
 * @brief A buffer handed back by a read or a write, with how much of it was used.
 *
 * What stream and file reads and writes, and `UdpSocket::recv()`, resolve with: `size`
 * bytes were read into `buf` or written from it. `auto [n, buf] = co_await f.read(...)`
 * binds it.
 *
 * Deliberately a plain aggregate, not a `std::pair`: GCC 15 never destroys a structured
 * binding through the tuple protocol whose hidden object lives in a coroutine frame across a
 * suspension, so a `std::pair` bound that way leaked its buffer (doc/known_issues.md, KI.2).
 * A plain aggregate is bound member by member, which GCC gets right. Don't give this type a
 * tuple protocol.
 *
 * @tparam Buf The buffer's type, a `ByteBuffer`.
 */
template<typename Buf>
struct BufResult {
    std::size_t size;
    Buf         buf;
};

} // namespace coro
