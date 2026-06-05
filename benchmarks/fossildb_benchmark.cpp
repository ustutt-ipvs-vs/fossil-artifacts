#include "db_benchmark_common.hpp"

#include <atomic>
#include <fossil/detail/nv_allocator.hpp>
#include <fossil/detail/pmap.hpp>
#include <fossil/reference.hpp>
#include <fossil/repository.hpp>
#include <fossil/transaction.hpp>

#include <string>
#include <vector>

namespace {

using namespace fossil::bench::db_benchmark;
using map_type = fossil::detail::pmap<key_type, std::string>;

class fossildb_backend
{
public:
    explicit fossildb_backend(const options& opts)
    {
        shards_.reserve(opts.shards);
        for(std::size_t i = 0; i < opts.shards; ++i) {
            shards_.push_back(fossil::object_repo().create<map_type>(1 << 20));
        }
    }

    void put(key_type key, std::string value)
    {
        auto idx = shard_of(key);
        auto& ref = shards_[idx];

        fossil::object_repo().single_write_transaction(ref, [&](map_type& map) {
            map.put(key, std::move(value));
        });
    }

    auto get(key_type key, std::string* value) -> bool
    {
        auto idx = shard_of(key);
        auto& ref = shards_[idx];

        auto found = false;

        fossil::object_repo().single_read_transaction(ref, [&](const map_type& map) {
            if(const auto* ptr = map.get(key)) {
                *value = *ptr;
                found = true;
            }
        });

        return found;
    }
private:
    constexpr auto shard_of(std::uint64_t id) -> std::uint64_t
    {
        auto hash = id;

        hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ul;
        hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebul;
        hash = hash ^ (hash >> 31);

        return hash % shards_.size();
    }

private:
    std::hash<key_type> hasher_{};
    std::vector<fossil::reference<map_type>> shards_;
};

} // namespace

auto main(int argc, const char* argv[]) -> int
{
    try {
        const auto opts = parse_cli(argc, argv, true);
        if(not opts.nvram_dir.empty()) {
            fossil::detail::nv_allocator::set_storage_directory(opts.nvram_dir);
            fossil::detail::nv_allocator::set_dax_mapping(true);
        }
        run_backend_benchmarks<fossildb_backend>("FossilDB", opts, true);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "fossildb benchmark failed: %s\n", error.what());
        return 1;
    }
}
