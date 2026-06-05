#pragma once

#include <atomic>
#include <cstdint>
#include <emmintrin.h>

namespace fossil::detail {
struct shared_lock_token
{
    std::uint64_t timestamp;
};

class optimistic_shared_mutex
{
public:
    void lock()
    {
        while(true) {
            auto prev = state_.exchange(true, std::memory_order_relaxed);
            if(!prev) {
                std::atomic_thread_fence(std::memory_order_acquire);
                return;
            }
            _mm_pause();
        }
    }

    void unlock() { state_.exchange(false, std::memory_order_release); }

    auto lock_shared() -> shared_lock_token
    {
        while(state_.load(std::memory_order_relaxed)) {
            _mm_pause();
        }
        return {clock_.load(std::memory_order_acquire)};
    }
    auto unlock_shared(shared_lock_token&& token) -> bool
    {
        auto now = clock_.load(std::memory_order_relaxed);
        return now == token.timestamp;
    }


private:
    std::atomic_uint64_t clock_;
    std::atomic_bool state_;
};
} // namespace fossil::detail
