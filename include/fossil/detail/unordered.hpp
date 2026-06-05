#pragma once

#include <ankerl/unordered_dense.h>

namespace fossil::detail {

template<typename Key, typename Value, typename Hash = ankerl::unordered_dense::hash<Key>>
using unordered_map = ankerl::unordered_dense::map<Key, Value, Hash>;

template<typename Key, typename Hash = ankerl::unordered_dense::hash<Key>>
using unordered_set = ankerl::unordered_dense::set<Key, Hash>;

} // namespace fossil::detail
