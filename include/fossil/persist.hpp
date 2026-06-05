#pragma once

#include "fossil/concepts.hpp"
#include <fossil/detail/pvec.hpp>
#include <type_traits>
#include <vector>

namespace fossil {

template<typename T>
class persist_impl
{
};


template<typename T>
requires(std::is_trivially_copyable_v<T> && not std::is_reference_v<T> && not std::is_pointer_v<T>)
class persist_impl<T>
{
public:
    using type = T;

    constexpr persist_impl() = default;

    persist_impl(const persist_impl&) = delete;
    auto operator=(const persist_impl&) -> persist_impl& = delete;

    persist_impl(persist_impl&&) = delete;
    auto operator=(persist_impl&&) -> persist_impl& = delete;

private:
    T cache_;
};


template<typename T>
using persist = std::conditional_t<is_specialization<T, std::vector>,
                                   detail::pvec<typename T::value_type>,
                                   persist_impl<T>>;

} // namespace fossil
