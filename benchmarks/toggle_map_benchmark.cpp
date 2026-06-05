#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fossil/detail/nv_allocator.hpp>
#include <fossil/detail/pRBTree.hpp>
#include <fossil/detail/pmap.hpp>
#include <fossil/reference.hpp>
#include <fossil/repository.hpp>
#include <fossil/transaction.hpp>
#include <map>
#include <mutex>
#include <print>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "fossil/detail/rw_spin_lock.hpp"
#include "histogram.hpp"
#include "measure_time.hpp"

#include <db.h>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <unistd.h>

#ifdef FOSSIL_HAS_PMDK
#include <libpmemobj.h>
#endif

namespace {

using key_type = std::int64_t;
using mapped_type = std::int64_t;
using pmap_type = fossil::detail::pmap<key_type, mapped_type>;
using prbmap_type = fossil::detail::pRBTree<key_type, mapped_type>;
using hist = fossil::bench::util::histogram;

enum class benchmark_kind {
    fossil_prbtree,
    fossil_pmap,
    fossil_sharded_pmap,
    std_map,
    std_unordered_map,
#ifdef FOSSIL_HAS_PMDK
    pmdk_hashmap,
    pmdk_rbtree,
#endif
    leveldb,
    bdb,
};

struct options
{
    std::size_t num_keys = 100'000;
    std::size_t num = 100'000;
    std::size_t threads = std::max(1u, std::thread::hardware_concurrency());
    std::size_t shards = 0;
    std::string pmem_dir;
    bool raw = false;
    bool volatile_pmdk = false;
    benchmark_kind benchmark = benchmark_kind::fossil_prbtree;
    bool benchmark_set = false;
};

auto parse_benchmark(std::string_view value) -> benchmark_kind
{
    if(value == "fossil-prbtree") {
        return benchmark_kind::fossil_prbtree;
    }
    if(value == "fossil-pmap") {
        return benchmark_kind::fossil_pmap;
    }
    if(value == "fossil-sharded-pmap") {
        return benchmark_kind::fossil_sharded_pmap;
    }
    if(value == "std-map") {
        return benchmark_kind::std_map;
    }
    if(value == "std-unordered-map") {
        return benchmark_kind::std_unordered_map;
    }
#ifdef FOSSIL_HAS_PMDK
    if(value == "pmdk-hashmap") {
        return benchmark_kind::pmdk_hashmap;
    }
    if(value == "pmdk-rbtree") {
        return benchmark_kind::pmdk_rbtree;
    }
#endif
    if(value == "leveldb") {
        return benchmark_kind::leveldb;
    }
    if(value == "bdb") {
        return benchmark_kind::bdb;
    }

#ifdef FOSSIL_HAS_PMDK
    throw std::runtime_error(
        "unknown benchmark; expected one of: fossil-prbtree, fossil-pmap, "
        "fossil-sharded-pmap, std-map, std-unordered-map, pmdk-hashmap, "
        "pmdk-rbtree, "
        "leveldb, bdb");
#else
    throw std::runtime_error(
        "unknown benchmark; expected one of: fossil-prbtree, fossil-pmap, "
        "fossil-sharded-pmap, std-map, std-unordered-map, leveldb, bdb");
#endif
}

auto parse_cli(int argc, const char* argv[]) -> options
{
    options opts;

    for(int i = 1; i < argc; ++i) {
        std::size_t value = 0;
        char junk = '\0';

        if(std::strncmp(argv[i], "--benchmark=", 12) == 0) {
            opts.benchmark = parse_benchmark(argv[i] + 12);
            opts.benchmark_set = true;
        } else if(std::sscanf(argv[i], "--num=%zu%c", &value, &junk) == 1) {
            opts.num = value;
        } else if(std::sscanf(argv[i], "--keys=%zu%c", &value, &junk) == 1) {
            opts.num_keys = value;
        } else if(std::sscanf(argv[i], "--threads=%zu%c", &value, &junk) == 1) {
            opts.threads = value;
        } else if(std::sscanf(argv[i], "--shards=%zu%c", &value, &junk) == 1) {
            opts.shards = value;
        } else if(std::strncmp(argv[i], "--nvram-dir=", 12) == 0) {
            opts.pmem_dir = argv[i] + 12;
        } else if(std::strcmp(argv[i], "--raw") == 0) {
            opts.raw = true;
        } else if(std::strcmp(argv[i], "--volatile-pmdk") == 0) {
            opts.volatile_pmdk = true;
        } else {
            throw std::runtime_error(std::format("invalid flag '{}'", argv[i]));
        }
    }

    if(not opts.benchmark_set) {
        throw std::runtime_error("--benchmark is required");
    }
    if(opts.num == 0) {
        throw std::runtime_error("--num must be greater than 0");
    }
    if(opts.num_keys == 0) {
        throw std::runtime_error("--keys must be greater than 0");
    }
    if(opts.threads == 0) {
        throw std::runtime_error("--threads must be greater than 0");
    }
    if(opts.shards == 0 && opts.benchmark == benchmark_kind::fossil_sharded_pmap) {
        opts.shards = std::min(opts.num, opts.threads);
    }
    if(opts.shards == 0 && opts.benchmark != benchmark_kind::fossil_sharded_pmap) {
        opts.shards = 1;
    }

    return opts;
}

auto benchmark_name(benchmark_kind benchmark, std::size_t shards) -> std::string
{
    switch(benchmark) {
    case benchmark_kind::fossil_prbtree: return "Fossil pRBTree";
    case benchmark_kind::fossil_pmap: return "Fossil pmap";
    case benchmark_kind::fossil_sharded_pmap: return std::format("Fossil sharded pmap x{}", shards);
    case benchmark_kind::std_map: return "std::map";
    case benchmark_kind::std_unordered_map: return "std::unordered_map";
#ifdef FOSSIL_HAS_PMDK
    case benchmark_kind::pmdk_hashmap: return "PMDK hashmap";
    case benchmark_kind::pmdk_rbtree: return "PMDK RBTree";
#endif
    case benchmark_kind::leveldb: return "LevelDB";
    case benchmark_kind::bdb: return "Berkeley DB";
    }

    std::unreachable();
}

auto active_threads_for(const options& opts) -> std::size_t
{
    return std::min(opts.num, opts.threads);
}

auto ops_for_thread(std::size_t thread_idx, std::size_t total_ops, std::size_t num_threads)
    -> std::size_t
{
    const auto base = total_ops / num_threads;
    const auto remainder = total_ops % num_threads;
    return base + (thread_idx < remainder ? 1 : 0);
}

auto random_key(std::size_t upper_inclusive) -> key_type
{
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    std::uniform_int_distribution<key_type> distribution(0, static_cast<key_type>(upper_inclusive));
    return distribution(generator);
}

auto validate_storage_dir(std::string_view label, const std::string& dir) -> std::filesystem::path
{
    auto path = std::filesystem::path(dir);
    if(not std::filesystem::exists(path)) {
        throw std::runtime_error(
            std::format("{} directory does not exist: {}", label, path.string()));
    }
    if(not std::filesystem::is_directory(path)) {
        throw std::runtime_error(
            std::format("{} path is not a directory: {}", label, path.string()));
    }

    return path;
}

void configure_fossil_for_benchmark(const options& opts)
{
    if(opts.pmem_dir.empty()) {
        return;
    }

    // The repository singleton constructs its PMEM shards on first use. Configure
    // the storage directory and DAX mapping before object_repo() is touched so
    // Fossil and PMDK use the same PMEM mount in NVM benchmark runs.
    fossil::detail::nv_allocator::set_storage_directory(
        validate_storage_dir("PMEM", opts.pmem_dir));
    fossil::detail::nv_allocator::set_dax_mapping(true);
}

void print_histogram(std::string_view name, const hist& histogram, bool raw)
{
    if(raw) {
        std::println("average={:.4f} median={:.4f} min={:.4f} max={:.4f} stddev={:.4f}",
                     histogram.average(),
                     histogram.median(),
                     histogram.min(),
                     histogram.max(),
                     histogram.standard_deviation());
        return;
    }

    std::println("{}\n{}", name, histogram.to_string());
}

template<typename F>
void run_parallel(std::size_t num_threads, std::string_view name, bool raw, F worker)
{
    std::vector<std::jthread> threads;
    threads.reserve(num_threads);

    std::vector<hist> histograms(num_threads);

    for(std::size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i] { worker(i, histograms[i]); });
    }

    for(auto& thread : threads) {
        thread.join();
    }

    for(std::size_t i = 1; i < num_threads; ++i) {
        histograms[0].merge(histograms[i]);
    }

    print_histogram(name, histograms[0], raw);
}

template<class Map>
void toggle_native(Map& map, fossil::detail::rw_spin_lock& mutex, key_type key)
{
    std::unique_lock guard{mutex};

    if(auto it = map.find(key); it != map.end()) {
        map.erase(it);
    } else {
        map.emplace(key, key);
    }
}

template<class Map>
void run_native_benchmark(const options& opts, std::string_view name)
{
    const auto num_threads = active_threads_for(opts);
    Map map;
    fossil::detail::rw_spin_lock mutex;

    run_parallel(num_threads, name, opts.raw, [&](std::size_t thread_idx, hist& histogram) {
        const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

        for(std::size_t i = 0; i < ops; ++i) {
            const auto time = fossil::bench::util::measure_time([&] {
                const auto key = random_key(opts.num_keys - 1);
                toggle_native(map, mutex, key);
            });

            histogram.add(time.count());
        }
    });
}

template<class Map>
void run_fossil_benchmark(const options& opts, std::string_view name)
{
    const auto num_threads = active_threads_for(opts);
    configure_fossil_for_benchmark(opts);
    auto ref = fossil::object_repo().create<Map>();

    run_parallel(num_threads, name, opts.raw, [&](std::size_t thread_idx, hist& histogram) {
        const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

        for(std::size_t i = 0; i < ops; ++i) {
            const auto time = fossil::bench::util::measure_time([&] {
                const auto key = random_key(opts.num_keys - 1);
                fossil::transaction(ref, [key](Map& map) {
                    if(map.contains(key)) {
                        map.remove(key);
                    } else {
                        map.put(key, key);
                    }
                });
            });

            histogram.add(time.count());
        }
    });
}

void run_fossil_sharded_pmap(const options& opts)
{
    const auto num_threads = active_threads_for(opts);
    const auto num_shards = opts.shards;

    configure_fossil_for_benchmark(opts);

    std::vector<fossil::reference<pmap_type>> refs;
    refs.reserve(num_shards);
    for(std::size_t i = 0; i < num_shards; ++i) {
        refs.emplace_back(fossil::object_repo().create<pmap_type>());
    }

    const auto name = benchmark_name(benchmark_kind::fossil_sharded_pmap, num_shards);

    run_parallel(num_threads, name, opts.raw, [&](std::size_t thread_idx, hist& histogram) {
        const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

        for(std::size_t i = 0; i < ops; ++i) {
            const auto time = fossil::bench::util::measure_time([&] {
                const auto key = random_key(opts.num_keys - 1);
                auto& ref = refs[static_cast<std::size_t>(key) % num_shards];

                fossil::transaction(ref, [key](pmap_type& map) {
                    if(map.contains(key)) {
                        map.remove(key);
                    } else {
                        map.put(key, key);
                    }
                });
            });

            histogram.add(time.count());
        }
    });
}

#ifdef FOSSIL_HAS_PMDK
[[noreturn]] void throw_pmdk_error(std::string_view action)
{
    const char* detail = pmemobj_errormsg();
    throw std::runtime_error(detail == nullptr ? std::string(action)
                                               : std::format("{}: {}", action, detail));
}

void setenv_or_throw(const char* name, const char* value)
{
    if(setenv(name, value, 1) != 0) {
        throw std::runtime_error(
            std::format("setenv({}, {}) failed: {}", name, value, std::strerror(errno)));
    }
}

void configure_pmdk_for_benchmark(const options& opts)
{
    if(not opts.volatile_pmdk) {
        return;
    }

    // The mmap-backed benchmark files are not real PMEM. Force libpmem onto the
    // pmem path and suppress cache flush instructions so this becomes a purely
    // volatile transaction benchmark instead of falling back to persistence work.
    setenv_or_throw("PMEM_IS_PMEM_FORCE", "1");
    setenv_or_throw("PMEM_NO_FLUSH", "1");
}

auto format_pmdk_benchmark_name(std::string_view base, const options& opts) -> std::string
{
    if(opts.volatile_pmdk) {
        return std::format("{} (volatile mmap)", base);
    }

    return std::string(base);
}

auto pmdk_pool_size() -> std::size_t
{
    return std::max<std::size_t>(PMEMOBJ_MIN_POOL, 256uz * 1024 * 1024);
}

auto resolve_pmdk_dir(const options& opts) -> std::filesystem::path
{
    return opts.pmem_dir.empty() ? std::filesystem::temp_directory_path()
                                 : validate_storage_dir("PMEM", opts.pmem_dir);
}

auto same_oid(PMEMoid lhs, PMEMoid rhs) -> bool
{
    return lhs.pool_uuid_lo == rhs.pool_uuid_lo && lhs.off == rhs.off;
}

enum class pmdk_slot_state : std::uint8_t {
    empty = 0,
    occupied = 1,
    tombstone = 2,
};

POBJ_LAYOUT_BEGIN(fossil_pmdk_hashmap_bench);
POBJ_LAYOUT_TOID(fossil_pmdk_hashmap_bench, struct pmdk_hashmap_slot);
POBJ_LAYOUT_TOID(fossil_pmdk_hashmap_bench, struct pmdk_hashmap);
POBJ_LAYOUT_ROOT(fossil_pmdk_hashmap_bench, struct pmdk_hashmap_root);
POBJ_LAYOUT_END(fossil_pmdk_hashmap_bench);

POBJ_LAYOUT_BEGIN(fossil_pmdk_rbtree_bench);
POBJ_LAYOUT_TOID(fossil_pmdk_rbtree_bench, struct pmdk_rbtree_node);
POBJ_LAYOUT_ROOT(fossil_pmdk_rbtree_bench, struct pmdk_rbtree_root);
POBJ_LAYOUT_END(fossil_pmdk_rbtree_bench);

struct pmdk_hashmap_slot
{
    key_type key = 0;
    mapped_type value = 0;
    pmdk_slot_state state = pmdk_slot_state::empty;
};

struct pmdk_hashmap
{
    std::size_t capacity = 0;
    std::size_t live_slots = 0;
    std::size_t tombstone_slots = 0;
    TOID(struct pmdk_hashmap_slot) slots;
};

struct pmdk_hashmap_root
{
    PMEMmutex lock;
    TOID(struct pmdk_hashmap) map;
};

enum class pmdk_rbtree_color : std::uint8_t {
    red = 0,
    black = 1,
};

struct pmdk_rbtree_node
{
    key_type key = 0;
    mapped_type value = 0;
    pmdk_rbtree_color color = pmdk_rbtree_color::black;
    TOID(struct pmdk_rbtree_node) left;
    TOID(struct pmdk_rbtree_node) right;
    TOID(struct pmdk_rbtree_node) parent;
};

struct pmdk_rbtree_root
{
    PMEMmutex lock;
    TOID(struct pmdk_rbtree_node) root;
};

class pmdk_hashmap_backend
{
public:
    explicit pmdk_hashmap_backend(const options& opts)
        : initial_capacity_(capacity_for_key_space(opts.num_keys))
    {
        configure_pmdk_for_benchmark(opts);

        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = resolve_pmdk_dir(opts) /
            std::format("fossil_toggle_pmdk_hashmap_{}_{}", timestamp, getpid());

        pop_ = pmemobj_create(path_.c_str(),
                              POBJ_LAYOUT_NAME(fossil_pmdk_hashmap_bench),
                              pmdk_pool_size(),
                              0600);
        if(pop_ == nullptr) {
            throw_pmdk_error("pmemobj_create failed");
        }

        root_ = POBJ_ROOT(pop_, struct pmdk_hashmap_root);
        initialize();
    }

    ~pmdk_hashmap_backend()
    {
        if(pop_ != nullptr) {
            pmemobj_close(pop_);
        }
        if(not path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }

    void toggle(key_type key)
    {
        int aborted = 0;

        TX_BEGIN_PARAM(pop_, TX_PARAM_MUTEX, &D_RW(root_)->lock)
        {
            auto* map_ptr = D_RW(D_RW(root_)->map);

            if((map_ptr->live_slots + map_ptr->tombstone_slots) * 2 >= map_ptr->capacity) {
                rehash(map_ptr);
            }

            const auto probe = find_slot(*map_ptr, key);
            auto* slots = D_RW(map_ptr->slots);

            if(probe.found) {
                TX_ADD_DIRECT(&slots[probe.index]);
                slots[probe.index].state = pmdk_slot_state::tombstone;
                TX_SET_DIRECT(map_ptr, live_slots, map_ptr->live_slots - 1);
                TX_SET_DIRECT(map_ptr, tombstone_slots, map_ptr->tombstone_slots + 1);
            } else {
                TX_ADD_DIRECT(&slots[probe.index]);
                slots[probe.index].key = key;
                slots[probe.index].value = key;
                slots[probe.index].state = pmdk_slot_state::occupied;
                TX_SET_DIRECT(map_ptr, live_slots, map_ptr->live_slots + 1);
                if(probe.reused_tombstone) {
                    TX_SET_DIRECT(map_ptr, tombstone_slots, map_ptr->tombstone_slots - 1);
                }
            }
        }
        TX_ONABORT { aborted = 1; }
        TX_END

        if(aborted != 0) {
            throw_pmdk_error("PMDK hashmap transaction failed");
        }
    }

private:
    static constexpr std::size_t min_initial_capacity_ = 16;

    struct probe_result
    {
        bool found = false;
        std::size_t index = 0;
        bool reused_tombstone = false;
    };

    static auto hash_key(key_type key) -> std::size_t { return std::hash<key_type>{}(key); }

    static auto capacity_for_key_space(std::size_t key_space) -> std::size_t
    {
        if(key_space > (std::numeric_limits<std::size_t>::max() - 1) / 2) {
            throw std::runtime_error("PMDK hashmap key space is too large");
        }

        const auto required = std::max(min_initial_capacity_, key_space * 2 + 1);
        return std::bit_ceil(required);
    }

    static void insert_without_snapshot(pmdk_hashmap_slot* slots,
                                        std::size_t capacity,
                                        key_type key,
                                        mapped_type value)
    {
        for(std::size_t probe = 0; probe < capacity; ++probe) {
            const auto index = (hash_key(key) + probe) % capacity;
            if(slots[index].state != pmdk_slot_state::occupied) {
                slots[index].key = key;
                slots[index].value = value;
                slots[index].state = pmdk_slot_state::occupied;
                return;
            }
        }

        throw std::runtime_error("PMDK hashmap rehash ran out of capacity");
    }

    static auto find_slot(const pmdk_hashmap& map, key_type key) -> probe_result
    {
        auto* slots = D_RO(map.slots);
        std::optional<std::size_t> tombstone_index;

        for(std::size_t probe = 0; probe < map.capacity; ++probe) {
            const auto index = (hash_key(key) + probe) % map.capacity;
            const auto state = slots[index].state;

            if(state == pmdk_slot_state::empty) {
                return {
                    .found = false,
                    .index = tombstone_index.value_or(index),
                    .reused_tombstone = tombstone_index.has_value(),
                };
            }

            if(state == pmdk_slot_state::tombstone) {
                if(not tombstone_index.has_value()) {
                    tombstone_index = index;
                }
                continue;
            }

            if(slots[index].key == key) {
                return {.found = true, .index = index, .reused_tombstone = false};
            }
        }

        if(tombstone_index.has_value()) {
            return {.found = false, .index = *tombstone_index, .reused_tombstone = true};
        }

        throw std::runtime_error("PMDK hashmap probe exhausted all slots");
    }

    void initialize()
    {
        int aborted = 0;

        TX_BEGIN(pop_)
        {
            auto* root_ptr = D_RW(root_);
            if(TOID_IS_NULL(root_ptr->map)) {
                TX_ADD_DIRECT(root_ptr);
                pmemobj_mutex_zero(pop_, &root_ptr->lock);
                root_ptr->map = TX_ZNEW(struct pmdk_hashmap);

                auto* map_ptr = D_RW(root_ptr->map);
                map_ptr->capacity = initial_capacity_;
                map_ptr->live_slots = 0;
                map_ptr->tombstone_slots = 0;
                map_ptr->slots = TX_ZALLOC(struct pmdk_hashmap_slot,
                                           map_ptr->capacity * sizeof(pmdk_hashmap_slot));
                if(TOID_IS_NULL(map_ptr->slots)) {
                    pmemobj_tx_abort(EINVAL);
                }
            }
        }
        TX_ONABORT { aborted = 1; }
        TX_END

        if(aborted != 0) {
            throw_pmdk_error("PMDK hashmap initialization failed");
        }
    }

    void rehash(pmdk_hashmap* map_ptr)
    {
        const auto should_grow = map_ptr->live_slots * 4 >= map_ptr->capacity;
        const auto new_capacity = should_grow
            ? std::max(min_initial_capacity_, map_ptr->capacity * 2)
            : map_ptr->capacity;
        const auto old_slots = map_ptr->slots;
        const auto old_capacity = map_ptr->capacity;

        auto new_slots = TX_ZALLOC(struct pmdk_hashmap_slot,
                                   new_capacity * sizeof(pmdk_hashmap_slot));
        if(TOID_IS_NULL(new_slots)) {
            pmemobj_tx_abort(EINVAL);
        }

        auto* new_data = D_RW(new_slots);
        auto* old_data = D_RO(old_slots);
        std::size_t live_slots = 0;

        for(std::size_t i = 0; i < old_capacity; ++i) {
            if(old_data[i].state == pmdk_slot_state::occupied) {
                insert_without_snapshot(new_data, new_capacity, old_data[i].key, old_data[i].value);
                ++live_slots;
            }
        }

        TX_SET_DIRECT(map_ptr, slots, new_slots);
        TX_SET_DIRECT(map_ptr, capacity, new_capacity);
        TX_SET_DIRECT(map_ptr, live_slots, live_slots);
        TX_SET_DIRECT(map_ptr, tombstone_slots, 0);
        TX_FREE(old_slots);
    }

    PMEMobjpool* pop_ = nullptr;
    TOID(struct pmdk_hashmap_root) root_;
    std::filesystem::path path_;
    std::size_t initial_capacity_ = min_initial_capacity_;
};

void run_pmdk_hashmap_benchmark(const options& opts)
{
    pmdk_hashmap_backend backend{opts};
    const auto num_threads = active_threads_for(opts);
    const auto name = format_pmdk_benchmark_name(benchmark_name(benchmark_kind::pmdk_hashmap,
                                                                opts.shards),
                                                 opts);

    run_parallel(num_threads, name, opts.raw, [&](std::size_t thread_idx, hist& histogram) {
        const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

        for(std::size_t i = 0; i < ops; ++i) {
            const auto time = fossil::bench::util::measure_time([&] {
                const auto key = random_key(opts.num_keys - 1);
                backend.toggle(key);
            });

            histogram.add(time.count());
        }
    });
}

class pmdk_rbtree_backend
{
    using node_oid = TOID(struct pmdk_rbtree_node);

public:
    explicit pmdk_rbtree_backend(const options& opts)
    {
        configure_pmdk_for_benchmark(opts);

        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = resolve_pmdk_dir(opts) /
            std::format("fossil_toggle_pmdk_rbtree_{}_{}", timestamp, getpid());

        pop_ = pmemobj_create(path_.c_str(),
                              POBJ_LAYOUT_NAME(fossil_pmdk_rbtree_bench),
                              pmdk_pool_size(),
                              0600);
        if(pop_ == nullptr) {
            throw_pmdk_error("pmemobj_create failed");
        }

        root_ = POBJ_ROOT(pop_, struct pmdk_rbtree_root);
        initialize();
    }

    ~pmdk_rbtree_backend()
    {
        if(pop_ != nullptr) {
            pmemobj_close(pop_);
        }
        if(not path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }

    void toggle(key_type key)
    {
        int aborted = 0;

        TX_BEGIN_PARAM(pop_, TX_PARAM_MUTEX, &D_RW(root_)->lock)
        {
            auto* root_ptr = D_RW(root_);
            const auto existing = find_node(root_ptr->root, key);
            if(TOID_IS_NULL(existing)) {
                insert(root_ptr, key, key);
            } else {
                erase(root_ptr, existing);
            }
        }
        TX_ONABORT { aborted = 1; }
        TX_END

        if(aborted != 0) {
            throw_pmdk_error("PMDK RBTree transaction failed");
        }
    }

private:
    static auto color_of(node_oid node) -> pmdk_rbtree_color
    {
        if(TOID_IS_NULL(node)) {
            return pmdk_rbtree_color::black;
        }
        return D_RO(node)->color;
    }

    static void set_color(node_oid node, pmdk_rbtree_color color)
    {
        if(not TOID_IS_NULL(node)) {
            TX_SET_DIRECT(D_RW(node), color, color);
        }
    }

    static auto minimum(node_oid node) -> node_oid
    {
        auto current = node;
        while(not TOID_IS_NULL(D_RO(current)->left)) {
            current = D_RO(current)->left;
        }
        return current;
    }

    static auto find_node(node_oid root, key_type key) -> node_oid
    {
        auto current = root;
        while(not TOID_IS_NULL(current)) {
            const auto* current_ptr = D_RO(current);
            if(key < current_ptr->key) {
                current = current_ptr->left;
            } else if(key > current_ptr->key) {
                current = current_ptr->right;
            } else {
                return current;
            }
        }
        return TOID_NULL(struct pmdk_rbtree_node);
    }

    static void left_rotate(pmdk_rbtree_root* root_ptr, node_oid x)
    {
        auto y = D_RO(x)->right;
        if(TOID_IS_NULL(y)) {
            return;
        }

        auto x_parent = D_RO(x)->parent;
        auto y_left = D_RO(y)->left;
        auto* x_ptr = D_RW(x);
        auto* y_ptr = D_RW(y);

        TX_SET_DIRECT(x_ptr, right, y_left);
        if(not TOID_IS_NULL(y_left)) {
            TX_SET_DIRECT(D_RW(y_left), parent, x);
        }

        TX_SET_DIRECT(y_ptr, parent, x_parent);
        if(TOID_IS_NULL(x_parent)) {
            TX_SET_DIRECT(root_ptr, root, y);
        } else if(same_oid(x.oid, D_RO(x_parent)->left.oid)) {
            TX_SET_DIRECT(D_RW(x_parent), left, y);
        } else {
            TX_SET_DIRECT(D_RW(x_parent), right, y);
        }

        TX_SET_DIRECT(y_ptr, left, x);
        TX_SET_DIRECT(x_ptr, parent, y);
    }

    static void right_rotate(pmdk_rbtree_root* root_ptr, node_oid y)
    {
        auto x = D_RO(y)->left;
        if(TOID_IS_NULL(x)) {
            return;
        }

        auto y_parent = D_RO(y)->parent;
        auto x_right = D_RO(x)->right;
        auto* y_ptr = D_RW(y);
        auto* x_ptr = D_RW(x);

        TX_SET_DIRECT(y_ptr, left, x_right);
        if(not TOID_IS_NULL(x_right)) {
            TX_SET_DIRECT(D_RW(x_right), parent, y);
        }

        TX_SET_DIRECT(x_ptr, parent, y_parent);
        if(TOID_IS_NULL(y_parent)) {
            TX_SET_DIRECT(root_ptr, root, x);
        } else if(same_oid(y.oid, D_RO(y_parent)->left.oid)) {
            TX_SET_DIRECT(D_RW(y_parent), left, x);
        } else {
            TX_SET_DIRECT(D_RW(y_parent), right, x);
        }

        TX_SET_DIRECT(x_ptr, right, y);
        TX_SET_DIRECT(y_ptr, parent, x);
    }

    static void insert_fixup(pmdk_rbtree_root* root_ptr, node_oid node)
    {
        auto current = node;

        while(true) {
            auto parent = D_RO(current)->parent;
            if(TOID_IS_NULL(parent) || color_of(parent) != pmdk_rbtree_color::red) {
                break;
            }

            auto grandparent = D_RO(parent)->parent;
            if(TOID_IS_NULL(grandparent)) {
                break;
            }

            if(same_oid(parent.oid, D_RO(grandparent)->left.oid)) {
                auto uncle = D_RO(grandparent)->right;
                if(color_of(uncle) == pmdk_rbtree_color::red) {
                    set_color(parent, pmdk_rbtree_color::black);
                    set_color(uncle, pmdk_rbtree_color::black);
                    set_color(grandparent, pmdk_rbtree_color::red);
                    current = grandparent;
                    continue;
                }

                if(same_oid(current.oid, D_RO(parent)->right.oid)) {
                    current = parent;
                    left_rotate(root_ptr, current);
                }

                auto current_parent = D_RO(current)->parent;
                auto current_grandparent = D_RO(current_parent)->parent;
                set_color(current_parent, pmdk_rbtree_color::black);
                set_color(current_grandparent, pmdk_rbtree_color::red);
                right_rotate(root_ptr, current_grandparent);
            } else {
                auto uncle = D_RO(grandparent)->left;
                if(color_of(uncle) == pmdk_rbtree_color::red) {
                    set_color(parent, pmdk_rbtree_color::black);
                    set_color(uncle, pmdk_rbtree_color::black);
                    set_color(grandparent, pmdk_rbtree_color::red);
                    current = grandparent;
                    continue;
                }

                if(same_oid(current.oid, D_RO(parent)->left.oid)) {
                    current = parent;
                    right_rotate(root_ptr, current);
                }

                auto current_parent = D_RO(current)->parent;
                auto current_grandparent = D_RO(current_parent)->parent;
                set_color(current_parent, pmdk_rbtree_color::black);
                set_color(current_grandparent, pmdk_rbtree_color::red);
                left_rotate(root_ptr, current_grandparent);
            }
        }

        set_color(root_ptr->root, pmdk_rbtree_color::black);
    }

    static void transplant(pmdk_rbtree_root* root_ptr, node_oid old_node, node_oid new_node)
    {
        auto parent = D_RO(old_node)->parent;
        if(TOID_IS_NULL(parent)) {
            TX_SET_DIRECT(root_ptr, root, new_node);
        } else if(same_oid(old_node.oid, D_RO(parent)->left.oid)) {
            TX_SET_DIRECT(D_RW(parent), left, new_node);
        } else {
            TX_SET_DIRECT(D_RW(parent), right, new_node);
        }

        if(not TOID_IS_NULL(new_node)) {
            TX_SET_DIRECT(D_RW(new_node), parent, parent);
        }
    }

    static void delete_fixup(pmdk_rbtree_root* root_ptr, node_oid node, node_oid parent)
    {
        auto current = node;
        auto current_parent = parent;

        while(not same_oid(current.oid, root_ptr->root.oid) &&
              color_of(current) == pmdk_rbtree_color::black) {
            if(TOID_IS_NULL(current_parent)) {
                break;
            }

            const auto current_is_left = same_oid(current.oid, D_RO(current_parent)->left.oid);
            auto sibling = current_is_left ? D_RO(current_parent)->right
                                           : D_RO(current_parent)->left;

            if(color_of(sibling) == pmdk_rbtree_color::red) {
                set_color(sibling, pmdk_rbtree_color::black);
                set_color(current_parent, pmdk_rbtree_color::red);
                if(current_is_left) {
                    left_rotate(root_ptr, current_parent);
                    sibling = D_RO(current_parent)->right;
                } else {
                    right_rotate(root_ptr, current_parent);
                    sibling = D_RO(current_parent)->left;
                }
            }

            auto sibling_left = TOID_IS_NULL(sibling) ? TOID_NULL(struct pmdk_rbtree_node)
                                                      : D_RO(sibling)->left;
            auto sibling_right = TOID_IS_NULL(sibling) ? TOID_NULL(struct pmdk_rbtree_node)
                                                       : D_RO(sibling)->right;

            if(color_of(sibling_left) == pmdk_rbtree_color::black &&
               color_of(sibling_right) == pmdk_rbtree_color::black) {
                set_color(sibling, pmdk_rbtree_color::red);
                current = current_parent;
                current_parent = D_RO(current_parent)->parent;
                continue;
            }

            if(current_is_left) {
                if(color_of(sibling_right) == pmdk_rbtree_color::black) {
                    set_color(sibling_left, pmdk_rbtree_color::black);
                    set_color(sibling, pmdk_rbtree_color::red);
                    right_rotate(root_ptr, sibling);
                    sibling = D_RO(current_parent)->right;
                }

                auto updated_right = TOID_IS_NULL(sibling) ? TOID_NULL(struct pmdk_rbtree_node)
                                                           : D_RO(sibling)->right;
                set_color(sibling, color_of(current_parent));
                set_color(current_parent, pmdk_rbtree_color::black);
                set_color(updated_right, pmdk_rbtree_color::black);
                left_rotate(root_ptr, current_parent);
            } else {
                if(color_of(sibling_left) == pmdk_rbtree_color::black) {
                    set_color(sibling_right, pmdk_rbtree_color::black);
                    set_color(sibling, pmdk_rbtree_color::red);
                    left_rotate(root_ptr, sibling);
                    sibling = D_RO(current_parent)->left;
                }

                auto updated_left = TOID_IS_NULL(sibling) ? TOID_NULL(struct pmdk_rbtree_node)
                                                          : D_RO(sibling)->left;
                set_color(sibling, color_of(current_parent));
                set_color(current_parent, pmdk_rbtree_color::black);
                set_color(updated_left, pmdk_rbtree_color::black);
                right_rotate(root_ptr, current_parent);
            }

            current = root_ptr->root;
            current_parent = TOID_NULL(struct pmdk_rbtree_node);
        }

        set_color(current, pmdk_rbtree_color::black);
    }

    static void erase(pmdk_rbtree_root* root_ptr, node_oid node)
    {
        auto removed_or_moved = node;
        auto removed_color = color_of(removed_or_moved);
        auto replacement = TOID_NULL(struct pmdk_rbtree_node);
        auto replacement_parent = TOID_NULL(struct pmdk_rbtree_node);

        if(TOID_IS_NULL(D_RO(node)->left)) {
            replacement = D_RO(node)->right;
            replacement_parent = D_RO(node)->parent;
            transplant(root_ptr, node, replacement);
        } else if(TOID_IS_NULL(D_RO(node)->right)) {
            replacement = D_RO(node)->left;
            replacement_parent = D_RO(node)->parent;
            transplant(root_ptr, node, replacement);
        } else {
            removed_or_moved = minimum(D_RO(node)->right);
            removed_color = color_of(removed_or_moved);
            replacement = D_RO(removed_or_moved)->right;

            if(same_oid(D_RO(removed_or_moved)->parent.oid, node.oid)) {
                replacement_parent = removed_or_moved;
                if(not TOID_IS_NULL(replacement)) {
                    TX_SET_DIRECT(D_RW(replacement), parent, removed_or_moved);
                }
            } else {
                replacement_parent = D_RO(removed_or_moved)->parent;
                transplant(root_ptr, removed_or_moved, replacement);
                auto moved_right = D_RO(node)->right;
                TX_SET_DIRECT(D_RW(removed_or_moved), right, moved_right);
                TX_SET_DIRECT(D_RW(moved_right), parent, removed_or_moved);
            }

            transplant(root_ptr, node, removed_or_moved);
            auto moved_left = D_RO(node)->left;
            TX_SET_DIRECT(D_RW(removed_or_moved), left, moved_left);
            TX_SET_DIRECT(D_RW(moved_left), parent, removed_or_moved);
            TX_SET_DIRECT(D_RW(removed_or_moved), color, D_RO(node)->color);
        }

        TX_FREE(node);

        if(removed_color == pmdk_rbtree_color::black) {
            delete_fixup(root_ptr, replacement, replacement_parent);
        }
    }

    static void insert(pmdk_rbtree_root* root_ptr, key_type key, mapped_type value)
    {
        auto parent = TOID_NULL(struct pmdk_rbtree_node);
        auto current = root_ptr->root;

        while(not TOID_IS_NULL(current)) {
            parent = current;
            if(key < D_RO(current)->key) {
                current = D_RO(current)->left;
            } else {
                current = D_RO(current)->right;
            }
        }

        auto inserted = TX_ZNEW(struct pmdk_rbtree_node);
        if(TOID_IS_NULL(inserted)) {
            pmemobj_tx_abort(EINVAL);
        }

        auto* inserted_ptr = D_RW(inserted);
        inserted_ptr->key = key;
        inserted_ptr->value = value;
        inserted_ptr->color = pmdk_rbtree_color::red;
        inserted_ptr->parent = parent;
        inserted_ptr->left = TOID_NULL(struct pmdk_rbtree_node);
        inserted_ptr->right = TOID_NULL(struct pmdk_rbtree_node);

        if(TOID_IS_NULL(parent)) {
            TX_SET_DIRECT(root_ptr, root, inserted);
        } else if(key < D_RO(parent)->key) {
            TX_SET_DIRECT(D_RW(parent), left, inserted);
        } else {
            TX_SET_DIRECT(D_RW(parent), right, inserted);
        }

        insert_fixup(root_ptr, inserted);
    }

    void initialize()
    {
        int aborted = 0;

        TX_BEGIN(pop_)
        {
            auto* root_ptr = D_RW(root_);
            TX_ADD_DIRECT(root_ptr);
            pmemobj_mutex_zero(pop_, &root_ptr->lock);
            root_ptr->root = TOID_NULL(struct pmdk_rbtree_node);
        }
        TX_ONABORT { aborted = 1; }
        TX_END

        if(aborted != 0) {
            throw_pmdk_error("PMDK RBTree initialization failed");
        }
    }

    PMEMobjpool* pop_ = nullptr;
    TOID(struct pmdk_rbtree_root) root_;
    std::filesystem::path path_;
};

void run_pmdk_rbtree_benchmark(const options& opts)
{
    pmdk_rbtree_backend backend{opts};
    const auto num_threads = active_threads_for(opts);
    const auto name = format_pmdk_benchmark_name(benchmark_name(benchmark_kind::pmdk_rbtree,
                                                                opts.shards),
                                                 opts);

    run_parallel(num_threads, name, opts.raw, [&](std::size_t thread_idx, hist& histogram) {
        const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

        for(std::size_t i = 0; i < ops; ++i) {
            const auto time = fossil::bench::util::measure_time([&] {
                const auto key = random_key(opts.num_keys - 1);
                backend.toggle(key);
            });

            histogram.add(time.count());
        }
    });
}

#endif

class leveldb_backend
{
public:
    leveldb_backend()
    {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            std::format("fossil_toggle_leveldb_{}_{}", timestamp, getpid());
        std::filesystem::create_directories(path_);

        leveldb::Options options;
        options.create_if_missing = true;
        options.error_if_exists = true;

        auto status = leveldb::DB::Open(options, path_.string(), &db_);
        if(not status.ok()) {
            throw std::runtime_error(status.ToString());
        }
    }

    ~leveldb_backend()
    {
        delete db_;
        if(not path_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }
    }

    void toggle(key_type key)
    {
        const auto key_bytes = std::bit_cast<std::array<char, sizeof(key_type)>>(key);
        const auto value_bytes = std::bit_cast<std::array<char, sizeof(mapped_type)>>(key);
        leveldb::Slice key_slice(key_bytes.data(), key_bytes.size());

        std::string value;
        const auto get_status = db_->Get(leveldb::ReadOptions(), key_slice, &value);
        if(not get_status.IsNotFound()) {
            if(not get_status.ok()) {
                throw std::runtime_error(get_status.ToString());
            }
        }

        // LevelDB does not expose transactions; WriteBatch is its atomic commit primitive.
        leveldb::WriteBatch batch;
        if(get_status.ok()) {
            batch.Delete(key_slice);
        } else {
            batch.Put(key_slice, leveldb::Slice(value_bytes.data(), value_bytes.size()));
        }

        const auto write_status = db_->Write(leveldb::WriteOptions(), &batch);
        if(not write_status.ok()) {
            throw std::runtime_error(write_status.ToString());
        }
    }

private:
    leveldb::DB* db_ = nullptr;
    std::filesystem::path path_;
};

void run_leveldb_benchmark(const options& opts)
{
    leveldb_backend backend;
    const auto num_threads = active_threads_for(opts);

    run_parallel(num_threads,
                 benchmark_name(benchmark_kind::leveldb, opts.shards),
                 opts.raw,
                 [&](std::size_t thread_idx, hist& histogram) {
                     const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

                     for(std::size_t i = 0; i < ops; ++i) {
                         const auto time = fossil::bench::util::measure_time([&] {
                             const auto key = random_key(opts.num_keys - 1);
                             backend.toggle(key);
                         });

                         histogram.add(time.count());
                     }
                 });
}

auto is_bdb_retryable(int rc) -> bool
{
    return rc == DB_LOCK_DEADLOCK || rc == DB_LOCK_NOTGRANTED || rc == DB_REP_HANDLE_DEAD;
}

class bdb_backend
{
public:
    bdb_backend()
    {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        env_home_ = std::filesystem::temp_directory_path() /
            std::format("fossil_toggle_bdb_{}_{}", timestamp, getpid());
        std::filesystem::create_directories(env_home_);

        const int create_env_rc = db_env_create(&env_, 0);
        if(create_env_rc != 0) {
            throw std::runtime_error(db_strerror(create_env_rc));
        }

        const int deadlock_detect_rc = env_->set_lk_detect(env_, DB_LOCK_DEFAULT);
        if(deadlock_detect_rc != 0) {
            env_->close(env_, 0);
            env_ = nullptr;
            throw std::runtime_error(db_strerror(deadlock_detect_rc));
        }

        const int open_env_rc = env_->open(env_,
                                           env_home_.c_str(),
                                           DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL |
                                               DB_INIT_TXN | DB_THREAD,
                                           0600);
        if(open_env_rc != 0) {
            env_->close(env_, 0);
            env_ = nullptr;
            throw std::runtime_error(db_strerror(open_env_rc));
        }

        const int create_rc = db_create(&db_, env_, 0);
        if(create_rc != 0) {
            env_->close(env_, 0);
            env_ = nullptr;
            throw std::runtime_error(db_strerror(create_rc));
        }

        const int open_rc = db_->open(db_,
                                      nullptr,
                                      "toggle.db",
                                      nullptr,
                                      DB_BTREE,
                                      DB_CREATE | DB_THREAD | DB_AUTO_COMMIT,
                                      0600);
        if(open_rc != 0) {
            db_->close(db_, 0);
            db_ = nullptr;
            env_->close(env_, 0);
            env_ = nullptr;
            throw std::runtime_error(db_strerror(open_rc));
        }
    }

    ~bdb_backend()
    {
        if(db_ != nullptr) {
            db_->close(db_, 0);
        }
        if(env_ != nullptr) {
            env_->close(env_, 0);
        }
        if(not env_home_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(env_home_, ec);
        }
    }

    void toggle(key_type key)
    {
        auto key_bytes = std::bit_cast<std::array<char, sizeof(key_type)>>(key);
        auto value_bytes = std::bit_cast<std::array<char, sizeof(mapped_type)>>(key);

        DBT key_dbt{};
        key_dbt.data = key_bytes.data();
        key_dbt.size = key_bytes.size();

        DBT insert_dbt{};
        insert_dbt.data = value_bytes.data();
        insert_dbt.size = value_bytes.size();

        while(true) {
            DB_TXN* txn = nullptr;
            const int begin_rc = env_->txn_begin(env_, nullptr, &txn, 0);
            if(begin_rc != 0) {
                throw std::runtime_error(db_strerror(begin_rc));
            }

            DBT value_dbt{};
            value_dbt.flags = DB_DBT_MALLOC;
            const int get_rc = db_->get(db_, txn, &key_dbt, &value_dbt, DB_RMW);
            if(get_rc == 0) {
                const int del_rc = db_->del(db_, txn, &key_dbt, 0);
                std::free(value_dbt.data);
                if(is_bdb_retryable(del_rc)) {
                    txn->abort(txn);
                    continue;
                }
                if(del_rc != 0) {
                    txn->abort(txn);
                    throw std::runtime_error(db_strerror(del_rc));
                }
            } else if(get_rc == DB_NOTFOUND) {
                const int put_rc = db_->put(db_, txn, &key_dbt, &insert_dbt, 0);
                if(is_bdb_retryable(put_rc)) {
                    txn->abort(txn);
                    continue;
                }
                if(put_rc != 0) {
                    txn->abort(txn);
                    throw std::runtime_error(db_strerror(put_rc));
                }
            } else if(is_bdb_retryable(get_rc)) {
                std::free(value_dbt.data);
                txn->abort(txn);
                continue;
            } else {
                std::free(value_dbt.data);
                txn->abort(txn);
                throw std::runtime_error(db_strerror(get_rc));
            }

            // Match the benchmark's non-sync durability semantics: log the commit,
            // but do not force a flush to stable storage on every toggle.
            const int commit_rc = txn->commit(txn, DB_TXN_WRITE_NOSYNC);
            if(is_bdb_retryable(commit_rc)) {
                continue;
            }
            if(commit_rc != 0) {
                throw std::runtime_error(db_strerror(commit_rc));
            }
            return;
        }
    }

private:
    DB_ENV* env_ = nullptr;
    DB* db_ = nullptr;
    std::filesystem::path env_home_;
};

void run_bdb_benchmark(const options& opts)
{
    bdb_backend backend;
    const auto num_threads = active_threads_for(opts);

    run_parallel(num_threads,
                 benchmark_name(benchmark_kind::bdb, opts.shards),
                 opts.raw,
                 [&](std::size_t thread_idx, hist& histogram) {
                     const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

                     for(std::size_t i = 0; i < ops; ++i) {
                         const auto time = fossil::bench::util::measure_time([&] {
                             const auto key = random_key(opts.num_keys - 1);
                             backend.toggle(key);
                         });

                         histogram.add(time.count());
                     }
                 });
}

} // namespace

auto main(int argc, const char* argv[]) -> int
{
    try {
        const auto opts = parse_cli(argc, argv);

        switch(opts.benchmark) {
        case benchmark_kind::fossil_prbtree:
            run_fossil_benchmark<prbmap_type>(opts,
                                              benchmark_name(benchmark_kind::fossil_prbtree,
                                                             opts.shards));
            break;
        case benchmark_kind::fossil_pmap:
            run_fossil_benchmark<pmap_type>(opts,
                                            benchmark_name(benchmark_kind::fossil_pmap,
                                                           opts.shards));
            break;
        case benchmark_kind::fossil_sharded_pmap: run_fossil_sharded_pmap(opts); break;
        case benchmark_kind::std_map:
            run_native_benchmark<std::map<key_type, mapped_type>>(
                opts,
                benchmark_name(benchmark_kind::std_map, opts.shards));
            break;
        case benchmark_kind::std_unordered_map:
            run_native_benchmark<std::unordered_map<key_type, mapped_type>>(
                opts,
                benchmark_name(benchmark_kind::std_unordered_map, opts.shards));
            break;
#ifdef FOSSIL_HAS_PMDK
        case benchmark_kind::pmdk_hashmap: run_pmdk_hashmap_benchmark(opts); break;
        case benchmark_kind::pmdk_rbtree: run_pmdk_rbtree_benchmark(opts); break;
#endif
        case benchmark_kind::leveldb: run_leveldb_benchmark(opts); break;
        case benchmark_kind::bdb: run_bdb_benchmark(opts); break;
        }

        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
