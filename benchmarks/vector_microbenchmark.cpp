#include <fossil/detail/rw_spin_lock.hpp>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fossil/detail/nv_allocator.hpp>
#include <fossil/detail/pvec.hpp>
#include <fossil/reference.hpp>
#include <fossil/repository.hpp>
#include <fossil/transaction.hpp>
#include <mutex>
#include <print>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef FOSSIL_HAS_PMDK
#include <chrono>
#include <libpmemobj.h>
#include <unistd.h>
#endif

#include "histogram.hpp"
#include "measure_time.hpp"

namespace {

using value_type = std::uint64_t;
using pvec_type = fossil::detail::pvec<value_type>;
using hist = fossil::bench::util::histogram;

enum class benchmark_kind {
    std_vector,
    fossil_pvec,
#ifdef FOSSIL_HAS_PMDK
    pmdk_vector,
#endif
};

struct options
{
    std::size_t num_keys = 1'000;
    std::size_t num = 100'000;
    std::size_t threads = std::max(1u, std::thread::hardware_concurrency());
    std::string pmem_dir;
    bool raw = false;
    bool volatile_pmdk = false;
    benchmark_kind benchmark = benchmark_kind::std_vector;
    bool benchmark_set = false;
};

auto parse_benchmark(std::string_view value) -> benchmark_kind
{
    if(value == "std-vector") {
        return benchmark_kind::std_vector;
    }
    if(value == "fossil-pvec") {
        return benchmark_kind::fossil_pvec;
    }
#ifdef FOSSIL_HAS_PMDK
    if(value == "pmdk-vector") {
        return benchmark_kind::pmdk_vector;
    }
#endif

#ifdef FOSSIL_HAS_PMDK
    throw std::runtime_error(
        "unknown benchmark; expected one of: std-vector, fossil-pvec, pmdk-vector");
#else
    throw std::runtime_error("unknown benchmark; expected one of: std-vector, fossil-pvec");
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

    return opts;
}

auto benchmark_name(benchmark_kind benchmark) -> std::string_view
{
    switch(benchmark) {
    case benchmark_kind::std_vector: return "std::vector";
    case benchmark_kind::fossil_pvec: return "Fossil pvec";
#ifdef FOSSIL_HAS_PMDK
    case benchmark_kind::pmdk_vector: return "PMDK vector";
#endif
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

auto random_value(std::size_t upper_inclusive) -> value_type
{
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    std::uniform_int_distribution<value_type> distribution(0,
                                                           static_cast<value_type>(
                                                               upper_inclusive));
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

template<typename Vec>
auto find_value_index(const Vec& vec, value_type value) -> std::size_t
{
    for(std::size_t i = 0; i < vec.size(); ++i) {
        if(vec[i] == value) {
            return i;
        }
    }

    return vec.size();
}

template<typename Vec>
void toggle_vector_value(Vec& vec, value_type value)
{
    const auto index = find_value_index(vec, value);
    if(index == vec.size()) {
        vec.emplace_back(value);
        return;
    }

    const auto last_index = vec.size() - 1;
    if(index != last_index) {
        std::swap(vec[index], vec[last_index]);
    }
    vec.pop_back();
}

void toggle_vector_value(pvec_type& vec, value_type value)
{
    const auto index = find_value_index(vec, value);
    if(index == vec.size()) {
        vec.emplace_back(value);
        return;
    }

    const auto last_index = vec.size() - 1;
    if(index != last_index) {
        vec.swap(index, last_index);
    }
    vec.pop_back();
}

#ifdef FOSSIL_HAS_PMDK
POBJ_LAYOUT_BEGIN(fossil_pmdk_vector_bench);
POBJ_LAYOUT_TOID(fossil_pmdk_vector_bench, struct pmdk_vector_value);
POBJ_LAYOUT_TOID(fossil_pmdk_vector_bench, struct pmdk_vector);
POBJ_LAYOUT_ROOT(fossil_pmdk_vector_bench, struct pmdk_vector_root);
POBJ_LAYOUT_END(fossil_pmdk_vector_bench);

struct pmdk_vector_value
{
    value_type value = 0;
};

struct pmdk_vector
{
    std::size_t size = 0;
    std::size_t capacity = 0;
    TOID(struct pmdk_vector_value) data;
};

struct pmdk_vector_root
{
    PMEMmutex lock;
    TOID(struct pmdk_vector) vec;
};

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

class pmdk_vector_backend
{
public:
    explicit pmdk_vector_backend(const options& opts)
    {
        configure_pmdk_for_benchmark(opts);

        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = resolve_pmdk_dir(opts) /
            std::format("fossil_vector_pmdk_{}_{}", timestamp, getpid());

        pop_ = pmemobj_create(path_.c_str(),
                              POBJ_LAYOUT_NAME(fossil_pmdk_vector_bench),
                              pmdk_pool_size(),
                              0600);
        if(pop_ == nullptr) {
            throw_pmdk_error("pmemobj_create failed");
        }

        root_ = POBJ_ROOT(pop_, struct pmdk_vector_root);
        initialize();
    }

    ~pmdk_vector_backend()
    {
        if(pop_ != nullptr) {
            pmemobj_close(pop_);
        }
        if(not path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }

    void toggle_value(value_type value)
    {
        int aborted = 0;

        TX_BEGIN_PARAM(pop_, TX_PARAM_MUTEX, &D_RW(root_)->lock)
        {
            auto vec = D_RW(root_)->vec;
            auto* vec_ptr = D_RW(vec);
            auto* current = D_RW(vec_ptr->data);

            std::size_t index = vec_ptr->size;
            for(std::size_t i = 0; i < vec_ptr->size; ++i) {
                if(current[i].value == value) {
                    index = i;
                    break;
                }
            }

            if(index == vec_ptr->size) {
                if(vec_ptr->size == vec_ptr->capacity) {
                    const auto new_capacity = std::max(8uz, vec_ptr->capacity * 2);
                    auto new_data = TX_ZREALLOC(vec_ptr->data,
                                                new_capacity * sizeof(pmdk_vector_value));
                    if(TOID_IS_NULL(new_data)) {
                        pmemobj_tx_abort(EINVAL);
                    }
                    TX_SET_DIRECT(vec_ptr, data, new_data);
                    TX_SET_DIRECT(vec_ptr, capacity, new_capacity);
                    current = D_RW(vec_ptr->data);
                }

                const auto index = vec_ptr->size;
                TX_ADD_DIRECT(&current[index]);
                current[index].value = value;
                TX_SET_DIRECT(vec_ptr, size, index + 1);
            } else {
                const auto last_index = vec_ptr->size - 1;
                if(index != last_index) {
                    const auto removed_value = current[index].value;
                    TX_ADD_DIRECT(&current[index]);
                    TX_ADD_DIRECT(&current[last_index]);
                    current[index].value = current[last_index].value;
                    current[last_index].value = removed_value;
                }
                TX_SET_DIRECT(vec_ptr, size, last_index);
            }
        }
        TX_ONABORT { aborted = 1; }
        TX_END

        if(aborted != 0) {
            throw_pmdk_error("PMDK vector transaction failed");
        }
    }

private:
    void initialize()
    {
        int aborted = 0;

        TX_BEGIN(pop_)
        {
            auto* root_ptr = D_RW(root_);
            if(TOID_IS_NULL(root_ptr->vec)) {
                TX_ADD_DIRECT(root_ptr);
                pmemobj_mutex_zero(pop_, &root_ptr->lock);
                root_ptr->vec = TX_ZNEW(struct pmdk_vector);

                auto* vec_ptr = D_RW(root_ptr->vec);
                vec_ptr->size = 0;
                vec_ptr->capacity = 8;
                vec_ptr->data = TX_ZALLOC(struct pmdk_vector_value,
                                          vec_ptr->capacity * sizeof(pmdk_vector_value));
                if(TOID_IS_NULL(vec_ptr->data)) {
                    pmemobj_tx_abort(EINVAL);
                }
            }
        }
        TX_ONABORT { aborted = 1; }
        TX_END

        if(aborted != 0) {
            throw_pmdk_error("PMDK vector initialization failed");
        }
    }

    PMEMobjpool* pop_ = nullptr;
    TOID(struct pmdk_vector_root) root_;
    std::filesystem::path path_;
};
#endif

void run_std_vector_benchmark(const options& opts)
{
    const auto num_threads = active_threads_for(opts);
    std::vector<value_type> vec;
    fossil::detail::rw_spin_lock mutex;

    run_parallel(num_threads,
                 benchmark_name(benchmark_kind::std_vector),
                 opts.raw,
                 [&](std::size_t thread_idx, hist& histogram) {
                     const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

                     for(std::size_t i = 0; i < ops; ++i) {
                         const auto time = fossil::bench::util::measure_time([&] {
                             const auto value = random_value(opts.num_keys - 1);
                             std::unique_lock guard{mutex};
                             toggle_vector_value(vec, value);
                         });

                         histogram.add(time.count());
                     }
                 });
}

void run_fossil_pvec_benchmark(const options& opts)
{
    const auto num_threads = active_threads_for(opts);
    configure_fossil_for_benchmark(opts);
    auto ref = fossil::object_repo().create<pvec_type>();

    run_parallel(num_threads,
                 benchmark_name(benchmark_kind::fossil_pvec),
                 opts.raw,
                 [&](std::size_t thread_idx, hist& histogram) {
                     const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

                     for(std::size_t i = 0; i < ops; ++i) {
                         const auto time = fossil::bench::util::measure_time([&] {
                             const auto value = random_value(opts.num_keys - 1);

                             fossil::transaction(ref, [value](pvec_type& vec) {
                                 toggle_vector_value(vec, value);
                             });
                         });

                         histogram.add(time.count());
                     }
                 });
}

#ifdef FOSSIL_HAS_PMDK
void run_pmdk_vector_benchmark(const options& opts)
{
    const auto num_threads = active_threads_for(opts);
    pmdk_vector_backend backend{opts};
    const auto name = format_pmdk_benchmark_name(benchmark_name(benchmark_kind::pmdk_vector), opts);

    run_parallel(num_threads, name, opts.raw, [&](std::size_t thread_idx, hist& histogram) {
        const auto ops = ops_for_thread(thread_idx, opts.num, num_threads);

        for(std::size_t i = 0; i < ops; ++i) {
            const auto time = fossil::bench::util::measure_time([&] {
                const auto value = random_value(opts.num_keys - 1);
                backend.toggle_value(value);
            });

            histogram.add(time.count());
        }
    });
}
#endif

} // namespace

auto main(int argc, const char* argv[]) -> int
{
    try {
        const auto opts = parse_cli(argc, argv);

        switch(opts.benchmark) {
        case benchmark_kind::std_vector: run_std_vector_benchmark(opts); break;
        case benchmark_kind::fossil_pvec: run_fossil_pvec_benchmark(opts); break;
#ifdef FOSSIL_HAS_PMDK
        case benchmark_kind::pmdk_vector: run_pmdk_vector_benchmark(opts); break;
#endif
        }

        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
