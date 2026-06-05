#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fossil/detail/pmap.hpp>
#include <fossil/detail/serialize.hpp>
#include <mutex>
#include <fossil/detail/tx_scope.hpp>
#include <fossil/is_resizable_object.hpp>
#include <fossil/repository.hpp>
#include <fossil/transaction.hpp>
#include <optional>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "histogram.hpp"
#include "measure_time.hpp"
#include "random.hpp"

constinit static std::size_t NUM_REPETITIONS = 100'000;
constinit static std::size_t NUM_SHARDS = 16;
constinit static std::size_t NUM_KEYS = 1000;
constinit static std::size_t NUM_THREADS = 16;

namespace {

template<class K, class V>
class map
{
public:
    using mapped_type = V;
    using key_type = K;

private:
public:
    constexpr map() noexcept
    {
        cache_.resize(1, std::nullopt);
        live_slots_ = 0;
        tombstone_slots_ = 0;
    }
    map(const map&) = delete;
    map(map&&) = default;
    auto operator=(const map&) -> map& = delete;
    auto operator=(map&&) -> map& = default;

    void clear()
    {
        cache_.clear();
        cache_.resize(1, std::nullopt);
        tombstone_slots_ = 0;
        live_slots_ = 0;
    }

    void put(K key, V value)
    {
        const auto cap = cache_.size();
        const auto used = live_slots_ + tombstone_slots_;

        if(used * 2 >= cap) {
            rehash();
        }

        put_without_rehash(std::move(key), std::move(value));
    }

    auto get(const K& key) -> mapped_type*
    {
        const auto hash = hasher(key);
        const auto size = cache_.size();
        auto probing = 0uz;

        while(true) {
            const auto idx = (hash + probing++) % size;
            auto& slot = cache_[idx];

            if(std::holds_alternative<tombstone>(slot)) {
                continue;
            }

            if(std::holds_alternative<std::nullopt_t>(slot)) {
                return nullptr;
            }

            if(auto& [slot_key, value] = std::get<std::pair<K, V>>(slot); slot_key == key) {
                return &value;
            }
        }
    }

    void remove(const K& key)
    {
        const auto hash = hasher(key);
        const auto size = cache_.size();
        auto probing = 0uz;

        while(true) {
            const auto idx = (hash + probing++) % size;
            const auto& slot = cache_[idx];

            if(std::holds_alternative<tombstone>(slot)) {
                continue;
            }

            if(std::holds_alternative<std::nullopt_t>(slot)) {
                return;
            }
            if(const auto& [slot_key, _] = std::get<std::pair<K, V>>(slot); slot_key == key) {
                cache_[idx] = tombstone{};
                live_slots_--;
                tombstone_slots_++;
                return;
            }
        }
    }

    // UNLOGGED MEMBERS
    auto size() const -> std::size_t { return live_slots_; }
    auto empty() const -> std::size_t { return live_slots_ == 0; }
    auto get(const K& key) const -> const mapped_type*
    {
        const auto hash = hasher(key);
        const auto size = cache_.size();
        auto probing = 0uz;

        while(true) {
            const auto& slot = cache_[(hash + probing++) % size];

            if(std::holds_alternative<tombstone>(slot)) {
                continue;
            }

            if(std::holds_alternative<std::nullopt_t>(slot)) {
                return nullptr;
            }

            if(const auto& [slot_key, value] = std::get<std::pair<K, V>>(slot); slot_key == key) {
                return &value;
            }
        }
    }

    auto contains(const K& key) const -> bool { return get(key) != nullptr; }

private:
    struct tombstone
    {
    };
    using cache_type = std::vector<std::variant<std::pair<K, V>, std::nullopt_t, tombstone>>;

    void rehash()
    {
        auto current = std::move(cache_);
        cache_.clear();
        cache_.resize(current.size() * 2, std::nullopt);
        live_slots_ = 0;
        tombstone_slots_ = 0;

        for(auto elem : std::move(current)) {
            if(auto* pair = std::get_if<std::pair<K, V>>(&elem)) {
                put_without_rehash(std::move(pair->first), std::move(pair->second));
            }
        }
    }

    void put_without_rehash(K key, V value)
    {
        const auto hash = hasher(key);
        const auto size = cache_.size();
        auto probing = 0uz;

        while(true) {
            const auto idx = (hash + probing++) % size;
            const auto& slot = cache_[idx];

            if(std::holds_alternative<tombstone>(slot)) {
                cache_[idx] = std::pair{std::move(key), std::move(value)};
                live_slots_++;
                tombstone_slots_--;
                return;
            }

            if(std::holds_alternative<std::nullopt_t>(slot)) {
                cache_[idx] = std::pair{std::move(key), std::move(value)};
                live_slots_++;
                return;
            }

            if(auto& [slot_key, value] = std::get<std::pair<K, V>>(slot); slot_key == key) {
                cache_[idx] = std::pair{std::move(key), std::move(value)};
                return;
            }
        }
    }


private:
    std::hash<K> hasher;
    cache_type cache_;

    std::size_t live_slots_;
    std::size_t tombstone_slots_;
};
} // namespace


template<typename F, typename... Args>
static void make_parallel(std::size_t num_threads, std::string name, F func, Args&&... args)
{
    std::vector<std::jthread> threads;
    threads.reserve(num_threads);

    std::vector<fossil::bench::util::histogram> histograms{num_threads};

    for(std::size_t i = 0; i < num_threads; ++i) {
        threads.push_back(std::jthread{func, i, std::ref(histograms[i]), std::ref(args)...});
    }

    for(std::size_t i = 0; i < num_threads; ++i) {
        threads[i].join();
    }

    for(std::size_t i = 1; i < num_threads; ++i) {
        histograms[0].merge(histograms[i]);
    }

    std::println("{}\n{}", name, histograms[0].to_string());
}

using hist = fossil::bench::util::histogram;

static void map_bench()
{
    using map_t = map<std::uint64_t, std::uint64_t>;
    map_t map;
    std::mutex mtx;

    make_parallel(
        NUM_THREADS,
        "map",
        [](auto j, hist& hist, map_t& map, std::mutex& mtx) {
            for(std::size_t i = 0; i < NUM_REPETITIONS; ++i) {
                auto time = fossil::bench::util::measure_time([&]() {
                    auto key = fossil::bench::util::random(0, NUM_KEYS - 1);
                    std::unique_lock guard{mtx};
                    map.put(key, 5);
                });
                hist.add(time.count());
            }
        },
        map,
        mtx);
}


using pv = fossil::detail::pmap<std::uint64_t, std::uint64_t>;

static void fossil_bench()
{
    auto ref = fossil::object_repo().create<pv>();

    make_parallel(
        NUM_THREADS,
        "Fossil",
        [](auto j, hist& hist, fossil::reference<pv> ref) {
            for(std::size_t i = 0; i < NUM_REPETITIONS; ++i) {
                auto time = fossil::bench::util::measure_time([&]() {
                    auto key = fossil::bench::util::random(0, NUM_KEYS - 1);
                    fossil::transaction(ref, [&](pv& vec) { vec.put(key, 5); });
                });
                hist.add(time.count());
            }
        },
        ref);
}


static void sharded_fossil_bench()
{
    std::vector<fossil::reference<pv>> refs;
    refs.reserve(NUM_SHARDS);
    for(std::size_t i = 0; i < NUM_SHARDS; ++i) {
        refs.emplace_back(fossil::object_repo().create<pv>());
    }

    make_parallel(
        NUM_THREADS,
        std::format("Fossil sharded x{}", NUM_SHARDS),
        [](auto j, hist& hist, std::vector<fossil::reference<pv>>& refs) {
            for(std::size_t i = 0; i < NUM_REPETITIONS; ++i) {
                auto time = fossil::bench::util::measure_time([&]() {
                    auto key = fossil::bench::util::random(0, NUM_KEYS - 1);
                    fossil::transaction(refs[key % NUM_SHARDS], [&](pv& vec) { vec.put(key, 5); });
                });
                hist.add(time.count());
            }
        },
        refs);
}


auto main() -> int
{
    map_bench();

    fossil_bench();

    sharded_fossil_bench();
}
