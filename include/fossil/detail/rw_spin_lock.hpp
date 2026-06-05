#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <emmintrin.h>
#include <stdatomic.h>

namespace fossil::detail {

/**
 * <h1> C-RW-WP </h1>
 *
 * A C-RW-WP reader-writer lock with writer preference and using a
 * Ticket Lock as Cohort.
 * This is starvation-free for writers and for readers, but readers may be
 * starved by writers.
 * C-RW-WP paper:         http://dl.acm.org/citation.cfm?id=2442532
 *
 * This variant of C-RW-WP has two modes on the writersMutex so that readers can
 * enter (but not writers). This is specific to DualZone because it's ok to let
 * the readers enter the 'main' while we're replicating modifications on 'back'.
 *
 */
class rw_spin_lock
{

private:
    class spin_lock
    {
        alignas(128) std::atomic<int> writers_{0};

    public:
        auto is_locked() -> bool { return (writers_.load() != 0); }
        void lock()
        {
            while(!try_lock())
                _mm_pause();
        }
        auto try_lock() -> bool
        {
            if(writers_.load(std::memory_order::relaxed) != 0)
                return false;
            int tmp = 0;
            return writers_.compare_exchange_strong(tmp,
                                                    2,
                                                    std::memory_order::acquire,
                                                    std::memory_order::relaxed);
        }
        void unlock() { writers_.store(0, std::memory_order_release); }
    };

    class ri_static_per_thread
    {
    private:
        static const uint64_t NOT_READING = 0;
        static const uint64_t READING = 1;
        static const int CLPAD = 128 / sizeof(uint64_t);
        alignas(128) std::atomic<uint64_t>* states_;

    public:
        constexpr static std::size_t REGISTRY_MAX_THREADS = 64;
        ri_static_per_thread()
        {
            states_ = new std::atomic<uint64_t>[REGISTRY_MAX_THREADS * CLPAD];
            for(int tid = 0; tid < REGISTRY_MAX_THREADS; tid++) {
                states_[static_cast<ptrdiff_t>(tid * CLPAD)].store(NOT_READING,
                                                                   std::memory_order::relaxed);
            }
        }

        ~ri_static_per_thread() { delete[] states_; }

        void arrive(const int tid) noexcept
        {
            states_[static_cast<ptrdiff_t>(tid * CLPAD)].store(READING, std::memory_order::acquire);
        }

        void depart(const int tid) noexcept
        {
            states_[static_cast<ptrdiff_t>(tid * CLPAD)].store(NOT_READING,
                                                               std::memory_order::release);
        }

        auto is_empty() noexcept -> bool
        {
            const int max_tid = REGISTRY_MAX_THREADS;
            for(int tid = 0; tid < max_tid; tid++) {
                if(states_[static_cast<ptrdiff_t>(tid * CLPAD)].load(std::memory_order::relaxed) != NOT_READING)
                    return false;
            }
            return true;
        }
    };

    spin_lock splock_{};
    ri_static_per_thread ri_;

    inline thread_local static int thread_id = -1;

    auto get_tid() -> int
    {
        if(thread_id == -1) [[unlikely]] {
            thread_id = thread_counter.fetch_add(1, memory_order_relaxed);
        }
        return thread_id;
    }

public:
    inline static std::atomic_int thread_counter = 0;

    void lock()
    {
        splock_.lock();
        while(!ri_.is_empty())
            _mm_pause();
    }

    auto try_lock() -> bool { return splock_.try_lock(); }

    void unlock() { splock_.unlock(); }

    void lock_shared()
    {
        auto tid = get_tid();
        while(true) {
            ri_.arrive(tid);
            if(!splock_.is_locked())
                break;
            ri_.depart(tid);
            while(splock_.is_locked())
                _mm_pause();
        }
    }

    void unlock_shared()
    {
        auto tid = get_tid();
        ri_.depart(tid);
    }
};


} // namespace fossil::detail
