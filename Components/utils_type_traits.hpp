#ifndef COMPONENTS_TYPE_TRAITS_HPP
#define COMPONENTS_TYPE_TRAITS_HPP

#include <cstddef>
#include <type_traits>

namespace gdut {

template <std::size_t Value> struct is_power_of_two {
  static constexpr bool value = Value && (Value & (Value - 1)) == 0;
};

template <std::size_t Value>
inline constexpr bool is_power_of_two_v = is_power_of_two<Value>::value;

template <typename> struct always_false : std::false_type {};

template <typename T>
inline constexpr bool always_false_v = always_false<T>::value;

} // namespace gdut

#endif // COMPONENTS_TYPE_TRAITS_HPP
