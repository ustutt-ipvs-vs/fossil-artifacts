#pragma once

#include <type_traits>

namespace fossil::detail {

template<typename T>
struct is_pointer_to_const
{
};

template<typename T>
struct is_pointer_to_const<const T* const> : std::true_type
{
};
template<typename T>
struct is_pointer_to_const<const T*> : std::true_type
{
};
template<typename T>
struct is_pointer_to_const<T* const> : std::false_type
{
};
template<typename T>
struct is_pointer_to_const<T*> : std::false_type
{
};

} // namespace fossil::detail
