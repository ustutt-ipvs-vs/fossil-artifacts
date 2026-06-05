#include "db_benchmark_common.hpp"

#include <shared_mutex>
#include <string>
#include <string_view>
#include <fossil/detail/unordered.hpp>
#include <fossil/detail/rw_spin_lock.hpp>

namespace {

using namespace fossil::bench::db_benchmark;

class unordered_map_rwlock_backend
{
public:
    explicit unordered_map_rwlock_backend(const options& opts)
    {
        // Avoid growth during the benchmark where possible.
        map_.reserve(opts.num);
    }

    void put(key_type key, std::string value)
    {
        std::unique_lock lock(mutex_);
        map_.insert_or_assign(key, std::move(value));
    }

    auto get(key_type key, std::string* value) -> bool
    {
        std::shared_lock lock(mutex_);
        const auto it = map_.find(key);
        if(it == map_.end()) {
            return false;
        }

        *value = it->second;
        return true;
    }

private:
    fossil::detail::unordered_map<key_type, std::string> map_;
    fossil::detail::rw_spin_lock mutex_;
};

} // namespace

auto main(int argc, const char* argv[]) -> int
{
    try {
        const auto opts = parse_cli(argc, argv, false);
        run_backend_benchmarks<unordered_map_rwlock_backend>("std::unordered_map + RW lock",
                                                             opts,
                                                             false);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "unordered_map_rwlock benchmark failed: %s\n", error.what());
        return 1;
    }
}
