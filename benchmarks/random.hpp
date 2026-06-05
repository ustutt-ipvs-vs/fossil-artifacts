#include <cstddef>
#include <limits>
#include <random>

namespace fossil::bench::util {

template<std::size_t low = std::numeric_limits<std::size_t>::min(),
         std::size_t up = std::numeric_limits<std::size_t>::max()>
inline auto random() noexcept -> std::size_t
{
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<std::size_t> distr(low, up);

    return distr(gen);
}

inline auto random(std::size_t low, std::size_t up) noexcept -> std::size_t
{
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<std::size_t> distr(low, up);

    return distr(gen);
}
} // namespace fossil::bench::util
