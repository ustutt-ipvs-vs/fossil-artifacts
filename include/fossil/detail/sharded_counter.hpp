#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fossil/concepts.hpp>
#include <fossil/detail/pmem.hpp>
#include <fossil/detail/tx_scope.hpp>
#include <fossil/is_resizable_object.hpp>
#include <fossil/reference.hpp>
#include <fossil/transaction_header.hpp>

namespace fossil {

template<std::uint64_t N>
class sharded_counter
{
    struct alignas(64) Shard
    {
        std::atomic<uint64_t> value;
    };

    std::array<Shard, N> shards_;

public:
    explicit sharded_counter(std::uint64_t initial = 0) noexcept
    {
        static_assert(std::has_single_bit(N), "N must be a power of 2");

        for(std::size_t i = 0; i < N; ++i) {
            shards_[i].value.store(initial + i, std::memory_order_relaxed);
        }
    }

    auto increment(std::uint64_t key) noexcept -> std::uint64_t
    {
        constexpr auto MASK = (N - 1);

        const auto shard = key & MASK;

        return shards_[shard].value.fetch_add(N, std::memory_order_relaxed);
    }
};

} // namespace fossil
