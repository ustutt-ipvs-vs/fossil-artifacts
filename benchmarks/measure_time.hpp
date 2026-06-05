#pragma once

#include <chrono>
#include <functional>

namespace fossil::bench::util {
template<typename Fn>
constexpr static auto measure_time(Fn&& function) -> std::chrono::nanoseconds
{
    using clock = std::chrono::high_resolution_clock;
    const auto start = clock::now();
    std::invoke(std::forward<Fn>(function));
    const auto end = clock::now();

    return {end - start};
}
} // namespace fossil::bench::util
