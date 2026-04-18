#pragma once

#include <concepts>
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

} // namespace coro
