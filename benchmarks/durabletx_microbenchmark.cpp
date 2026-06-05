#include "histogram.hpp"
#include "measure_time.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <new>
#include <print>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// TMRedBlackTree's destructor uses a dependent non-template updateTx call with
// the C++ template disambiguator. Keep the compatibility shim local so the
// durabletx submodule stays untouched.
#define updateTx(...) updateTx<bool>(__VA_ARGS__)
#include "../durabletx/pdatastructures/TMRedBlackTree.hpp"
#undef updateTx

namespace {

#ifndef FOSSIL_DURABLETX_BACKEND_NAME
#error "FOSSIL_DURABLETX_BACKEND_NAME must be defined by the unity benchmark"
#endif

using value_type = std::uint64_t;
using hist = fossil::bench::util::histogram;
using selected_tm = FOSSIL_DURABLETX_TM;

template<typename T>
using selected_persist = FOSSIL_DURABLETX_PERSIST<T>;

enum class benchmark_kind {
    rb_tree,
    vector,
    hashmap,
};

struct options
{
    std::size_t num_keys = 1'000;
    std::size_t num = 100'000;
    std::size_t threads = std::max(1u, std::thread::hardware_concurrency());
    benchmark_kind benchmark = benchmark_kind::rb_tree;
    bool benchmark_set = false;
    bool raw = false;
};

auto parse_benchmark(std::string_view value) -> benchmark_kind
{
    if(value == "rb-tree" || value == "rbtree") {
        return benchmark_kind::rb_tree;
    }
    if(value == "vector") {
        return benchmark_kind::vector;
    }
    if(value == "hashmap" || value == "hash-map" || value == "map") {
        return benchmark_kind::hashmap;
    }

    throw std::runtime_error("unknown benchmark; expected one of: rb-tree, vector, hashmap");
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
        } else if(std::strcmp(argv[i], "--raw") == 0) {
            opts.raw = true;
        } else if(std::strncmp(argv[i], "--nvram-dir=", 12) == 0) {
            // Consumed by PM_FILE_NAME before main through /proc/self/cmdline.
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

    return opts;
}

auto benchmark_name(benchmark_kind benchmark) -> std::string_view
{
    switch(benchmark) {
    case benchmark_kind::rb_tree: return "RBTree";
    case benchmark_kind::vector: return "Vector";
    case benchmark_kind::hashmap: return "HashMap";
    }

    std::unreachable();
}

auto active_threads_for(const options& opts) -> std::size_t
{
    return std::min(opts.num, opts.threads);
}

auto ops_for_thread(std::size_t thread_idx, std::size_t total_ops, std::size_t num_threads) -> std::size_t
{
    const auto base = total_ops / num_threads;
    const auto remainder = total_ops % num_threads;
    return base + (thread_idx < remainder ? 1 : 0);
}

auto random_value(std::size_t upper_inclusive) -> value_type
{
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    std::uniform_int_distribution<value_type> distribution(0, static_cast<value_type>(upper_inclusive));
    return distribution(generator);
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

template<typename T>
auto get_root(int idx) -> T*
{
#ifdef FOSSIL_DURABLETX_ROMULUS
    return selected_tm::template get_object<T>(idx);
#else
    return static_cast<T*>(selected_tm::get_object(idx));
#endif
}

template<typename T>
void put_root(int idx, T* object)
{
#ifdef FOSSIL_DURABLETX_ROMULUS
    selected_tm::template put_object<T>(idx, object);
#else
    selected_tm::put_object(idx, object);
#endif
}

template<typename Object>
class durabletx_root
{
public:
    template<typename... Args>
    explicit durabletx_root(Args&&... args)
    {
        selected_tm::updateTx([&] {
            object_ = selected_tm::template tmNew<Object>(std::forward<Args>(args)...);
            put_root(0, object_);
        });
    }

    durabletx_root(const durabletx_root&) = delete;
    auto operator=(const durabletx_root&) -> durabletx_root& = delete;

    ~durabletx_root()
    {
        try {
            selected_tm::updateTx([&] {
                put_root<Object>(0, nullptr);
            });
        } catch(...) {
        }

#ifdef PM_FILE_NAME
        std::error_code error;
        std::filesystem::remove(PM_FILE_NAME, error);
#endif
    }

    auto get() const -> Object* { return object_; }

private:
    Object* object_ = nullptr;
};

template<typename T, typename TM, template<typename> class P>
class TMVectorSet
{
public:
    explicit TMVectorSet(std::size_t initial_capacity = 8)
    {
        capacity_ = initial_capacity;
        size_ = 0;
        values_ = allocate(initial_capacity);
    }

    ~TMVectorSet()
    {
        if(values_ != nullptr) {
            TM::pfree(values_);
        }
    }

    auto insert_if_missing(T value) -> bool
    {
        if(contains(value)) {
            return false;
        }

        const auto size = static_cast<std::size_t>(size_);
        if(size == static_cast<std::size_t>(capacity_)) {
            grow();
        }

        auto* values = static_cast<P<T>*>(values_);
        values[size] = value;
        size_ = size + 1;
        return true;
    }

private:
    auto contains(T value) -> bool
    {
        auto* values = static_cast<P<T>*>(values_);
        const auto size = static_cast<std::size_t>(size_);

        for(std::size_t i = 0; i < size; ++i) {
            if(static_cast<T>(values[i]) == value) {
                return true;
            }
        }

        return false;
    }

    static auto allocate(std::size_t capacity) -> P<T>*
    {
        auto* values = static_cast<P<T>*>(TM::pmalloc(capacity * sizeof(P<T>)));
        for(std::size_t i = 0; i < capacity; ++i) {
            values[i] = T{};
        }
        return values;
    }

    void grow()
    {
        const auto old_size = static_cast<std::size_t>(size_);
        const auto old_capacity = static_cast<std::size_t>(capacity_);
        const auto new_capacity = std::max<std::size_t>(8, old_capacity * 2);
        auto* old_values = static_cast<P<T>*>(values_);
        auto* new_values = allocate(new_capacity);

        for(std::size_t i = 0; i < old_size; ++i) {
            new_values[i] = static_cast<T>(old_values[i]);
        }

        TM::pfree(old_values);
        values_ = new_values;
        capacity_ = new_capacity;
    }

    P<std::size_t> size_ = 0;
    P<std::size_t> capacity_ = 0;
    P<T>* values_ = nullptr;
};

template<typename K, typename V, typename TM, template<typename> class P>
class TMSimpleHashMap
{
public:
    explicit TMSimpleHashMap(std::size_t capacity = 65'536)
    {
        capacity_ = capacity;
        size_ = 0;
        slots_ = static_cast<Slot*>(TM::pmalloc(capacity * sizeof(Slot)));
        for(std::size_t i = 0; i < capacity; ++i) {
            new (&slots_[i]) Slot();
        }
    }

    void put(const K& key, const V& value)
    {
        const auto capacity = static_cast<std::size_t>(capacity_);
        const auto start = std::hash<K>{}(key) % capacity;
        Slot* first_deleted = nullptr;

        for(std::size_t offset = 0; offset < capacity; ++offset) {
            auto& slot = slots_[(start + offset) % capacity];
            const auto state = static_cast<std::uint8_t>(slot.state);
            if(state == deleted && first_deleted == nullptr) {
                first_deleted = &slot;
                continue;
            }

            if(state == empty) {
                auto& target = first_deleted == nullptr ? slot : *first_deleted;
                target.key = key;
                target.value = value;
                target.state = occupied;
                size_ = static_cast<std::size_t>(size_) + 1;
                return;
            }

            if(state == occupied && static_cast<K>(slot.key) == key) {
                slot.value = value;
                return;
            }
        }

        if(first_deleted != nullptr) {
            first_deleted->key = key;
            first_deleted->value = value;
            first_deleted->state = occupied;
            size_ = static_cast<std::size_t>(size_) + 1;
            return;
        }

        throw std::runtime_error("open-address hashmap is full");
    }

    auto contains(const K& key) -> bool
    {
        return find_slot(key) != nullptr;
    }

    auto remove(const K& key) -> bool
    {
        auto* slot = find_slot(key);
        if(slot == nullptr) {
            return false;
        }

        slot->state = deleted;
        size_ = static_cast<std::size_t>(size_) - 1;
        return true;
    }

private:
    static constexpr std::uint8_t empty = 0;
    static constexpr std::uint8_t occupied = 1;
    static constexpr std::uint8_t deleted = 2;

    struct Slot
    {
        P<std::uint8_t> state = empty;
        P<K> key;
        P<V> value;
    };

    auto find_slot(const K& key) -> Slot*
    {
        const auto capacity = static_cast<std::size_t>(capacity_);
        const auto start = std::hash<K>{}(key) % capacity;

        for(std::size_t offset = 0; offset < capacity; ++offset) {
            auto& slot = slots_[(start + offset) % capacity];
            const auto state = static_cast<std::uint8_t>(slot.state);
            if(state == empty) {
                return nullptr;
            }
            if(state == occupied && static_cast<K>(slot.key) == key) {
                return &slot;
            }
        }

        return nullptr;
    }

    P<std::size_t> size_ = 0;
    P<std::size_t> capacity_ = 0;
    Slot* slots_ = nullptr;
};

using rbtree_type =
    TMRedBlackTree<value_type, value_type, selected_tm, selected_persist>;
using hashmap_type =
    TMSimpleHashMap<value_type, value_type, selected_tm, selected_persist>;
using vector_type =
    TMVectorSet<value_type, selected_tm, selected_persist>;

void run_rbtree_benchmark(const options& opts)
{
    const auto num_threads = active_threads_for(opts);
    durabletx_root<rbtree_type> root;
    const auto name = std::format("{} {}", FOSSIL_DURABLETX_BACKEND_NAME, benchmark_name(opts.benchmark));

    run_parallel(num_threads, name, opts.raw, [&](std::size_t thread_idx, hist& histogram) {
        const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

        for(std::size_t i = 0; i < ops; ++i) {
            const auto time = fossil::bench::util::measure_time([&] {
                const auto key = random_value(opts.num_keys - 1);
                selected_tm::updateTx([&] {
                    value_type not_used{};
                    if(root.get()->innerGet(key, not_used, false)) {
                        root.get()->innerRemove(key);
                    } else {
                        root.get()->innerPut(key, key);
                    }
                });
            });

            histogram.add(time.count());
        }
    });
}

void run_vector_benchmark(const options& opts)
{
    const auto num_threads = active_threads_for(opts);
    durabletx_root<vector_type> root;
    const auto name = std::format("{} {}", FOSSIL_DURABLETX_BACKEND_NAME, benchmark_name(opts.benchmark));

    run_parallel(num_threads, name, opts.raw, [&](std::size_t thread_idx, hist& histogram) {
        const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

        for(std::size_t i = 0; i < ops; ++i) {
            const auto time = fossil::bench::util::measure_time([&] {
                const auto value = random_value(opts.num_keys - 1);
                selected_tm::updateTx([&] { root.get()->insert_if_missing(value); });
            });

            histogram.add(time.count());
        }
    });
}

void run_hashmap_benchmark(const options& opts)
{
    const auto num_threads = active_threads_for(opts);
    if(opts.num_keys > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::runtime_error("hashmap key space is too large");
    }

    const auto target_capacity = opts.num_keys * 2;
    auto capacity = std::size_t{8};
    while(capacity < target_capacity) {
        if(capacity > std::numeric_limits<std::size_t>::max() / 2) {
            throw std::runtime_error("hashmap key space is too large");
        }
        capacity *= 2;
    }

    durabletx_root<hashmap_type> root{capacity};
    const auto name = std::format("{} {}", FOSSIL_DURABLETX_BACKEND_NAME, benchmark_name(opts.benchmark));

    run_parallel(num_threads, name, opts.raw, [&](std::size_t thread_idx, hist& histogram) {
        const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

        for(std::size_t i = 0; i < ops; ++i) {
            const auto time = fossil::bench::util::measure_time([&] {
                const auto key = random_value(opts.num_keys - 1);
                selected_tm::updateTx([&] {
                    if(root.get()->contains(key)) {
                        root.get()->remove(key);
                    } else {
                        root.get()->put(key, key);
                    }
                });
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
        case benchmark_kind::rb_tree: run_rbtree_benchmark(opts); break;
        case benchmark_kind::vector: run_vector_benchmark(opts); break;
        case benchmark_kind::hashmap: run_hashmap_benchmark(opts); break;
        }

        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,
                     "%s microbenchmark failed: %s\n",
                     FOSSIL_DURABLETX_BACKEND_NAME,
                     error.what());
        return 1;
    }
}
