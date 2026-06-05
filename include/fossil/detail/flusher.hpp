#include <atomic>
#include <fossil/detail/nv_allocator.hpp>

namespace fossil::detail {

class flusher
{
public:
    void flush()
    {
        auto epoch = epoch_.load(std::memory_order_relaxed);

        is_flushing_.wait(true);

        if(is_flushing_.exchange(true, std::memory_order_relaxed)) {
            // somebody else is flushing

            if(not will_be_flushing_.exchange(true, std::memory_order_relaxed)) {
                // I will be the one to flush

                alloc_->flush();
                will_be_flushing_.store(false, std::memory_order_relaxed);
            } else {
                // somebody else will flush for me.
                // There is nothing to do.
                return;
            }
        } else {
            // nobody else is flushing.
            alloc_->flush();
        }

        is_flushing_.store(false, std::memory_order_relaxed);
    }


private:
    std::atomic_bool is_flushing_ = false;
    std::atomic_uint64_t epoch_ = 0;

    nv_allocator* alloc_;
};

} // namespace fossil::detail
