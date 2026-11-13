#pragma once

// For the checks that a result type coro hands back is bound member by member rather than
// through the tuple protocol: GCC 15 leaks a structured binding through the tuple protocol
// when its hidden object is in a coroutine frame across a suspension (doc/known_issues.md,
// KI.2). Each such type is a plain aggregate without std::tuple_size; these checks keep it so.

#include <tuple>
#include <type_traits>

namespace coro_test {

template<typename T>
concept HasTupleProtocol = requires { std::tuple_size<T>::value; };

/// True for a type a structured binding unpacks member by member, as GCC gets right.
template<typename T>
inline constexpr bool kBindsMemberwise = std::is_aggregate_v<T> && !HasTupleProtocol<T>;

} // namespace coro_test
