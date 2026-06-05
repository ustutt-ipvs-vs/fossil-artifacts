#pragma once

#include <array>
#include <concepts>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <vector>


template<class T, class V>
concept range_of = std::ranges::range<T> &&
    std::same_as<V, std::remove_const_t<std::ranges::range_value_t<T>>>;


template<class, template<class...> class>
inline constexpr bool is_specialization = false;
template<template<class...> class T, class... Args>
inline constexpr bool is_specialization<T<Args...>, T> = true;

template<class T>
concept is_vector = is_specialization<T, std::vector>;

template<class T>
concept is_optional = is_specialization<T, std::optional>;

template<class T>
concept is_tuple = is_specialization<T, std::tuple>;

template<class P>
concept is_pair_like = requires(P pair_like) {
    std::get<0>(pair_like);
    std::get<1>(pair_like);
};


namespace impl {

template<class T>
concept simple_serializable = std::is_arithmetic_v<T> or std::is_pointer_v<T> or
    std::same_as<T, std::byte> or std::is_arithmetic_v<std::underlying_type_t<T>> or
    std::same_as<std::underlying_type_t<T>, std::byte>;

// clang-format off
template<class T>
concept custom_serializable = requires(const T& t, std::span<const std::byte>& span)
{
    {T::deserialize(span)} -> std::same_as<T>;
    {T::serialize(t)} -> range_of<std::byte>;
};

template <class T>
struct SerializableRecursionHelperTrait : std::conditional_t<custom_serializable<T> or simple_serializable<T>,
                                                             std::true_type,
                                                             std::false_type> {};

template<>
struct SerializableRecursionHelperTrait<std::string> : std::true_type{};

template <class T, class A>
struct SerializableRecursionHelperTrait<std::vector<T, A>> : SerializableRecursionHelperTrait<T> {};

template <class T, auto N>
struct SerializableRecursionHelperTrait<std::array<T, N>> : SerializableRecursionHelperTrait<T> {};

template <class T>
struct SerializableRecursionHelperTrait<std::optional<T>> : SerializableRecursionHelperTrait<T> {};

template <class ... Args>
struct SerializableRecursionHelperTrait<std::tuple<Args...>> : std::conjunction<SerializableRecursionHelperTrait<Args>...> {};

template <class T>
struct SerializableRecursionHelperTrait<const T> : SerializableRecursionHelperTrait<T> {};
// clang-format on

} // namespace impl


template<class T>
concept SimpleSerializable = impl::simple_serializable<std::remove_const_t<T>>;

template<class T>
concept CustomSerializable = impl::custom_serializable<std::remove_const_t<T>>;

template<class T>
concept Serializable = impl::SerializableRecursionHelperTrait<T>::value;
