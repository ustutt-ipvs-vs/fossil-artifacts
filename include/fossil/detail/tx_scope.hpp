#pragma once

#include <fossil/detail/serialize.hpp>
#include <fossil/reference.hpp>
#include <type_traits>

namespace fossil {

template<typename T>
struct tx_scope
{
    using object_type = std::remove_const_t<T>;

    // this transaction
    std::uint64_t tx_id;
    reference<T> object;
    std::uint64_t shard = 0;
    object_type* object_ptr = nullptr;

    // if this transaction is nested inside another, the outer tx is set
    std::optional<std::uint64_t> outer_tx;

    constexpr auto is_nested() const -> bool { return outer_tx.has_value(); }
};


} // namespace fossil
