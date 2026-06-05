#pragma once

#include <fossil/detail/function_traits.hpp>
#include <type_traits>

namespace fossil {

template<typename Tx>
struct transaction_traits
{
    using object_ref = detail::first_argument_t<Tx>;
    using object_type = std::remove_reference_t<object_ref>;
    constexpr static bool is_read_only = std::is_const_v<object_type>;
};

} // namespace fossil
