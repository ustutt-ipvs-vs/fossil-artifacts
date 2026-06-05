#include <cstddef>
#include <cstdint>
#include <format>
#include <fossil/detail/pRBTree.hpp>
#include <fossil/repository.hpp>
#include <fossil/transaction.hpp>
#include <map>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "histogram.hpp"
#include "measure_time.hpp"
#include "random.hpp"

constinit static std::size_t NUM_REPETITIONS = 1'000'000;
constinit static std::size_t NUM_KEYS = 1000;
constinit static std::size_t NUM_THREADS = 8;

namespace {

template<class K, class V>
class rb_tree
{
public:
    using mapped_type = V;
    using key_type = K;

    rb_tree() = default;
    rb_tree(const rb_tree&) = delete;
    rb_tree(rb_tree&&) = default;
    auto operator=(const rb_tree&) -> rb_tree& = delete;
    auto operator=(rb_tree&&) -> rb_tree& = default;

    void clear() { cache_.clear(); }

    void put(K key, V value) { cache_.insert_or_assign(std::move(key), std::move(value)); }

    auto get(const K& key) -> mapped_type*
    {
        auto it = cache_.find(key);
        if(it == cache_.end()) {
            return nullptr;
        }

        return &it->second;
    }

    void remove(const K& key) { cache_.erase(key); }

    auto size() const -> std::size_t { return cache_.size(); }
    auto empty() const -> std::size_t { return cache_.empty(); }

    auto get(const K& key) const -> const mapped_type*
    {
        auto it = cache_.find(key);
        if(it == cache_.end()) {
            return nullptr;
        }

        return &it->second;
    }

    auto contains(const K& key) const -> bool { return get(key) != nullptr; }

private:
    std::map<K, V> cache_;
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

static void rbtree_bench()
{
    using tree_t = rb_tree<std::uint64_t, std::uint64_t>;
    tree_t tree;
    std::mutex mtx;

    make_parallel(
        NUM_THREADS,
        "RBTree",
        [](auto j, hist& hist, tree_t& tree, std::mutex& mtx) {
            (void)j;
            for(std::size_t i = 0; i < NUM_REPETITIONS; ++i) {
                auto time = fossil::bench::util::measure_time([&]() {
                    auto key = fossil::bench::util::random(0, NUM_KEYS - 1);
                    std::unique_lock guard{mtx};
                    tree.put(key, 5);
                });
                hist.add(time.count());
            }
        },
        tree,
        mtx);
}

using pt = fossil::detail::pRBTree<std::uint64_t, std::uint64_t>;

static void fossil_bench()
{
    auto ref = fossil::object_repo().create<pt>();

    make_parallel(
        NUM_THREADS,
        "Fossil RBTree",
        [](auto j, hist& hist, fossil::reference<pt> ref) {
            (void)j;
            for(std::size_t i = 0; i < NUM_REPETITIONS; ++i) {
                auto time = fossil::bench::util::measure_time([&]() {
                    auto key = fossil::bench::util::random(0, NUM_KEYS - 1);
                    fossil::transaction(ref, [&](pt& tree) { tree.put(key, 5); });
                });
                hist.add(time.count());
            }
        },
        ref);
}

auto main() -> int
{
    rbtree_bench();
    fossil_bench();
}
