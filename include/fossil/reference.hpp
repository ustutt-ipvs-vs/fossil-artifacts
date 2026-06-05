#pragma once

#include <atomic>
#include <cstdint>
#include <fossil/detail/serialize.hpp>

namespace fossil {

template<typename T>
struct reference
{

    reference() : id(0) {}
    explicit reference(std::uint64_t id) : id(id) {}

    reference(const reference& other) : id(other.id), raw(other.raw.load(std::memory_order_relaxed))
    {}
    reference(reference&& other) noexcept
        : id(other.id), raw(other.raw.load(std::memory_order_relaxed))
    {}

    auto operator=(const reference& other) -> reference&
    {
        if(this != &other) {
            id = other.id;
            raw.store(other.raw.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    };
    auto operator=(reference&&) -> reference& = delete;

    using value_type = T;

    std::uint64_t id;
    std::atomic<T*> raw = nullptr;


    auto serialize() const noexcept { return ::serialize(id); }
};

} // namespace fossil
