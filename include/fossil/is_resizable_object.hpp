#pragma once

#include <concepts>
#include <cstddef>
#include <vector>

namespace fossil {

template<typename T>
concept is_resizable_object = requires(T t) {
    typename T::log_type;
    { t.serialize() } -> std::same_as<std::vector<std::byte>>;
    { t.current_log() } -> std::same_as<const std::vector<std::byte>&>;
    { t.clean_log() } -> std::same_as<void>;
};


} // namespace fossil
