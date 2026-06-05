#pragma once

#include <bit>
#include <fossil/concepts.hpp>
#include <fossil/detail/serialize.hpp>
#include <vector>

namespace fossil::detail {

struct transaction_header
{
    const constinit static inline std::uint64_t TX_MAGIC = 0xCCCCCCCCCCCCCCCC;
    constinit static std::uint64_t TX_HEADER_SIZE;
    const constinit static inline std::uint64_t NO_CONDITON_TX = 0x0000000000000000;

    std::uint64_t magic = TX_MAGIC;

    std::uint64_t tx_id;
    std::uint64_t condition_tx_id = NO_CONDITON_TX;

    auto serialize() const noexcept -> std::array<std::byte, 24>;
};

inline auto transaction_header::serialize() const noexcept -> std::array<std::byte, 24>
{
    static_assert(sizeof(transaction_header) == 24);
    return std::bit_cast<std::array<std::byte, 24>>(*this);
}

constinit inline std::uint64_t transaction_header::TX_HEADER_SIZE = sizeof(transaction_header);

} // namespace fossil::detail
