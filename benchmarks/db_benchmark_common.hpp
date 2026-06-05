#pragma once

#include "histogram.hpp"
#include "measure_time.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fossil/detail/rw_spin_lock.hpp>
#include <fstream>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

namespace fossil::bench::db_benchmark {

using histogram = fossil::bench::util::histogram;
using key_type = std::uint64_t;

enum class benchmark_id {
    fill_seq,
    fill_random,
    override_existing,
    read_seq,
    read_random,
    read_write
};

struct benchmark_spec
{
    benchmark_id id;
    std::string name;
};

struct options
{
    std::size_t num = 1'000'000;
    std::size_t reads = 0;
    std::size_t threads = std::max(1u, std::thread::hardware_concurrency());
    std::size_t value_size = 100;
    std::size_t shards = 0;
    std::string benchmarks = "fill-seq,fill-random,override,read-seq,read-random,read-write";
    std::string nvram_dir;
    bool raw = false;
};

struct work_range
{
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct benchmark_result
{
    histogram hist;
    double wall_time_ns = 0.0;
    double throughput_ops_per_s = 0.0;
};

inline auto splitmix64(std::uint64_t value) -> std::uint64_t
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

inline void print_usage(const char* argv0, bool allow_shards)
{
    std::fprintf(stderr,
                 "Usage: %s [--benchmarks=list] [--num=N] [--reads=N] [--threads=N] "
                 "[--value_size=N] [--nvram-dir=DIR]%s [--raw]\n",
                 argv0,
                 allow_shards ? " [--shards=N]" : "");
}

inline auto command_line_nvram_dir() -> std::string
{
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    if(not cmdline) {
        return {};
    }

    std::vector<std::string> args;
    std::string current;
    char ch = '\0';
    while(cmdline.get(ch)) {
        if(ch == '\0') {
            args.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if(not current.empty()) {
        args.push_back(std::move(current));
    }

    for(std::size_t i = 1; i < args.size(); ++i) {
        if(args[i].rfind("--nvram-dir=", 0) == 0) {
            return args[i].substr(12);
        }
        if(args[i] == "--nvram-dir" && i + 1 < args.size()) {
            return args[i + 1];
        }
    }

    return {};
}

inline auto validate_storage_dir(std::string_view dir) -> std::filesystem::path
{
    auto path = std::filesystem::path(dir);
    if(not std::filesystem::exists(path)) {
        throw std::runtime_error("NVRAM directory does not exist: " + path.string());
    }
    if(not std::filesystem::is_directory(path)) {
        throw std::runtime_error("NVRAM path is not a directory: " + path.string());
    }
    return path;
}

inline auto benchmark_storage_dir(const options& opts) -> std::filesystem::path
{
    if(opts.nvram_dir.empty()) {
        return std::filesystem::temp_directory_path();
    }
    return validate_storage_dir(opts.nvram_dir);
}

inline auto make_benchmark_path(const options& opts, std::string_view prefix)
    -> std::filesystem::path
{
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return benchmark_storage_dir(opts) /
        (std::string(prefix) + "_" + std::to_string(timestamp) + "_" + std::to_string(getpid()));
}

inline auto durabletx_pmem_file_name(const char* default_path) -> const char*
{
    static const std::string path = [default_path] {
        const auto dir = command_line_nvram_dir();
        if(dir.empty()) {
            return std::string(default_path);
        }

        const auto leaf = std::filesystem::path(default_path).filename().string();
        return (std::filesystem::path(dir) / (leaf + "_" + std::to_string(getpid()))).string();
    }();
    return path.c_str();
}

inline auto parse_benchmark_name(std::string_view name) -> benchmark_spec
{
    if(name == "fill-seq" || name == "fillseq") {
        return {benchmark_id::fill_seq, "fill-seq"};
    }
    if(name == "fill-random" || name == "fillrandom") {
        return {benchmark_id::fill_random, "fill-random"};
    }
    if(name == "override" || name == "overwrite") {
        return {benchmark_id::override_existing, "override"};
    }
    if(name == "read-seq" || name == "readseq") {
        return {benchmark_id::read_seq, "read-seq"};
    }
    if(name == "read-random" || name == "readrandom") {
        return {benchmark_id::read_random, "read-random"};
    }
    if(name == "read-write" || name == "readwrite" || name == "read-write-50" ||
       name == "readwrite50") {
        return {benchmark_id::read_write, "read-write"};
    }

    throw std::runtime_error("unknown benchmark '" + std::string(name) + "'");
}

inline auto parse_benchmarks(std::string_view list) -> std::vector<benchmark_spec>
{
    std::vector<benchmark_spec> benchmarks;

    while(not list.empty()) {
        const auto comma = list.find(',');
        const auto token = comma == std::string_view::npos ? list : list.substr(0, comma);

        if(not token.empty()) {
            benchmarks.push_back(parse_benchmark_name(token));
        }

        if(comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }

    if(benchmarks.empty()) {
        throw std::runtime_error("no benchmarks selected");
    }

    return benchmarks;
}

inline auto parse_cli(int argc, const char* argv[], bool allow_shards) -> options
{
    options opts;

    for(int i = 1; i < argc; ++i) {
        std::size_t value = 0;
        char junk = '\0';

        if(std::strncmp(argv[i], "--benchmarks=", 13) == 0) {
            opts.benchmarks = argv[i] + 13;
        } else if(std::sscanf(argv[i], "--num=%zu%c", &value, &junk) == 1) {
            opts.num = value;
        } else if(std::sscanf(argv[i], "--reads=%zu%c", &value, &junk) == 1) {
            opts.reads = value;
        } else if(std::sscanf(argv[i], "--threads=%zu%c", &value, &junk) == 1) {
            opts.threads = value;
        } else if(std::sscanf(argv[i], "--value_size=%zu%c", &value, &junk) == 1) {
            opts.value_size = value;
        } else if(std::strncmp(argv[i], "--nvram-dir=", 12) == 0) {
            opts.nvram_dir = argv[i] + 12;
        } else if(std::strcmp(argv[i], "--nvram-dir") == 0) {
            if(i + 1 >= argc) {
                print_usage(argv[0], allow_shards);
                throw std::runtime_error("missing directory after '" + std::string(argv[i]) + "'");
            }
            opts.nvram_dir = argv[++i];
        } else if(allow_shards && std::sscanf(argv[i], "--shards=%zu%c", &value, &junk) == 1) {
            opts.shards = value;
        } else if(std::strcmp(argv[i], "--raw") == 0) {
            opts.raw = true;
        } else {
            print_usage(argv[0], allow_shards);
            throw std::runtime_error("invalid flag '" + std::string(argv[i]) + "'");
        }
    }

    if(opts.num == 0) {
        throw std::runtime_error("--num must be greater than 0");
    }
    if(opts.reads == 0) {
        opts.reads = opts.num;
    }
    if(opts.threads == 0) {
        throw std::runtime_error("--threads must be greater than 0");
    }
    if(allow_shards) {
        if(opts.shards == 0) {
            opts.shards = std::min(opts.num, opts.threads);
        }
        if(opts.shards == 0) {
            opts.shards = 1;
        }
    } else if(opts.shards != 0) {
        throw std::runtime_error("--shards is not supported by this benchmark");
    }
    if(not opts.nvram_dir.empty()) {
        validate_storage_dir(opts.nvram_dir);
    }

    return opts;
}

inline void print_header(std::string_view backend_name, const options& opts, bool show_shards)
{
    std::fprintf(stdout,
                 "%.*s benchmark\n",
                 static_cast<int>(backend_name.size()),
                 backend_name.data());
    std::fprintf(stdout, "entries:    %zu\n", opts.num);
    std::fprintf(stdout, "reads:      %zu\n", opts.reads);
    std::fprintf(stdout, "threads:    %zu\n", opts.threads);
    std::fprintf(stdout, "value_size: %zu\n", opts.value_size);
    if(show_shards) {
        std::fprintf(stdout, "shards:     %zu\n", opts.shards);
    }
    if(not opts.nvram_dir.empty()) {
        std::fprintf(stdout, "nvram_dir:  %s\n", opts.nvram_dir.c_str());
    }
    std::fprintf(stdout, "----------------------------------------\n");
}

inline auto total_ops(const options& opts, benchmark_id id) -> std::size_t
{
    switch(id) {
    case benchmark_id::fill_seq:
    case benchmark_id::fill_random:
    case benchmark_id::override_existing: return opts.num;
    case benchmark_id::read_seq:
    case benchmark_id::read_random:
    case benchmark_id::read_write: return opts.reads;
    }

    std::abort();
}

inline auto active_threads(const options& opts, benchmark_id id) -> std::size_t
{
    return std::max<std::size_t>(1, std::min(opts.threads, total_ops(opts, id)));
}

inline auto range_for_thread(std::size_t thread_idx, std::size_t total, std::size_t num_threads)
    -> work_range
{
    const auto base = total / num_threads;
    const auto extra = total % num_threads;
    const auto begin = thread_idx * base + std::min(thread_idx, extra);
    const auto count = base + (thread_idx < extra ? 1 : 0);
    return {begin, begin + count};
}

inline auto permuted_index(std::size_t index, std::size_t count, std::uint64_t seed) -> std::size_t
{
    if(count <= 1) {
        return 0;
    }

    const auto offset = splitmix64(seed) % count;
    auto stride = splitmix64(seed ^ 0xd1b54a32d192ed03ULL) % count;
    if(stride == 0) {
        stride = 1;
    }
    while(std::gcd(stride, count) != 1) {
        ++stride;
        if(stride == count) {
            stride = 1;
        }
    }

    return (offset + (index * stride) % count) % count;
}

inline auto random_index(std::size_t op_index, std::size_t count, std::uint64_t seed) -> std::size_t
{
    if(count <= 1) {
        return 0;
    }
    return splitmix64(seed + op_index) % count;
}

inline auto encode_key(key_type key) -> std::array<char, sizeof(key_type)>
{
    std::array<char, sizeof(key_type)> bytes{};
    for(std::size_t i = 0; i < sizeof(key_type); ++i) {
        const auto shift = static_cast<unsigned>((sizeof(key_type) - 1 - i) * 8);
        bytes[i] = static_cast<char>((key >> shift) & 0xffU);
    }
    return bytes;
}

inline auto make_value(key_type key, std::size_t value_size, std::uint64_t salt) -> std::string
{
    std::string value(value_size, '\0');
    auto state = splitmix64(key ^ salt);

    for(std::size_t i = 0; i < value.size(); ++i) {
        if((i % 8) == 0) {
            state = splitmix64(state + i + salt);
        }
        value[i] = static_cast<char>('a' + (state & 0x0fU));
        state >>= 4;
    }

    return value;
}

inline void clear_histogram(histogram& hist) { hist.clear(); }

inline void print_histogram(std::string_view name, const benchmark_result& result, bool raw)
{
    if(raw) {
        std::fprintf(stdout,
                     "%.*s average_ns=%.4f median_ns=%.4f min_ns=%.4f max_ns=%.4f "
                     "stddev_ns=%.4f wall_time_ns=%.4f throughput_ops_per_s=%.4f\n",
                     static_cast<int>(name.size()),
                     name.data(),
                     result.hist.average(),
                     result.hist.median(),
                     result.hist.min(),
                     result.hist.max(),
                     result.hist.standard_deviation(),
                     result.wall_time_ns,
                     result.throughput_ops_per_s);
        return;
    }

    std::fprintf(stdout,
                 "%-12.*s : %11.3f ns/op\n",
                 static_cast<int>(name.size()),
                 name.data(),
                 result.hist.average());
    std::fprintf(stdout, "Throughput  : %11.3f ops/s\n", result.throughput_ops_per_s);
    std::fprintf(stdout, "Nanoseconds per op:\n%s\n", result.hist.to_string().c_str());
}

template<typename Worker>
auto run_parallel(std::size_t num_threads,
                  std::size_t total_ops,
                  std::string_view name,
                  bool raw,
                  Worker worker) -> benchmark_result
{
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    std::vector<histogram> histograms(num_threads);
    for(auto& histogram : histograms) {
        clear_histogram(histogram);
    }

    std::exception_ptr error;
    std::mutex error_mutex;

    const auto wall_time = fossil::bench::util::measure_time([&] {
        for(std::size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back([&, i] {
                try {
                    worker(i, histograms[i]);
                } catch(...) {
                    std::lock_guard lock(error_mutex);
                    if(error == nullptr) {
                        error = std::current_exception();
                    }
                }
            });
        }

        for(auto& thread : threads) {
            thread.join();
        }
    });

    if(error != nullptr) {
        std::rethrow_exception(error);
    }

    for(std::size_t i = 1; i < histograms.size(); ++i) {
        histograms[0].merge(histograms[i]);
    }

    benchmark_result result;
    result.hist = std::move(histograms[0]);
    result.wall_time_ns = static_cast<double>(wall_time.count());
    if(result.wall_time_ns > 0.0) {
        result.throughput_ops_per_s = static_cast<double>(total_ops) * 1'000'000'000.0 /
            result.wall_time_ns;
    }

    print_histogram(name, result, raw);
    return result;
}

template<typename Backend>
void prefill(Backend& backend, const options& opts)
{
    for(std::size_t i = 0; i < opts.num; ++i) {
        const auto key = static_cast<key_type>(i);
        backend.put(key, make_value(key, opts.value_size, 0));
    }
}

template<typename Backend>
void run_benchmark_case(Backend& backend, const options& opts, const benchmark_spec& benchmark)
{
    const auto num_threads = active_threads(opts, benchmark.id);
    const auto total = total_ops(opts, benchmark.id);

    run_parallel(num_threads,
                 total,
                 benchmark.name,
                 opts.raw,
                 [&](std::size_t thread_idx, histogram& hist) {
                     const auto work = range_for_thread(thread_idx, total, num_threads);

                     for(std::size_t op = work.begin; op < work.end; ++op) {
                         switch(benchmark.id) {
                         case benchmark_id::fill_seq: {
                             const auto key = static_cast<key_type>(op);
                             const auto value = make_value(key, opts.value_size, 0);
                             const auto elapsed = fossil::bench::util::measure_time(
                                 [&] { backend.put(key, std::move(value)); });
                             hist.add(elapsed.count());
                             break;
                         }
                         case benchmark_id::fill_random: {
                             const auto key = static_cast<key_type>(
                                 permuted_index(op, opts.num, 0x91e10da5c79e7b1dULL));
                             const auto value = make_value(key, opts.value_size, 0);
                             const auto elapsed = fossil::bench::util::measure_time(
                                 [&] { backend.put(key, std::move(value)); });
                             hist.add(elapsed.count());
                             break;
                         }
                         case benchmark_id::override_existing: {
                             const auto key = static_cast<key_type>(
                                 random_index(op, opts.num, 0x4f1bbcdc6762f4b5ULL));
                             const auto value = make_value(key, opts.value_size, 1);
                             const auto elapsed = fossil::bench::util::measure_time(
                                 [&] { backend.put(key, std::move(value)); });
                             hist.add(elapsed.count());
                             break;
                         }
                         case benchmark_id::read_seq: {
                             const auto key = static_cast<key_type>(op % opts.num);
                             std::string value;
                             const auto elapsed = fossil::bench::util::measure_time([&] {
                                 if(not backend.get(key, &value)) {
                                     throw std::runtime_error("missing key during read-seq");
                                 }
                             });
                             hist.add(elapsed.count());
                             break;
                         }
                         case benchmark_id::read_random: {
                             const auto key = static_cast<key_type>(
                                 random_index(op, opts.num, 0xc949d7c7509e6557ULL));
                             std::string value;
                             const auto elapsed = fossil::bench::util::measure_time([&] {
                                 if(not backend.get(key, &value)) {
                                     throw std::runtime_error("missing key during read-random");
                                 }
                             });
                             hist.add(elapsed.count());
                             break;
                         }
                         case benchmark_id::read_write: {
                             if((op & 1U) == 0) {
                                 const auto key = static_cast<key_type>(
                                     random_index(op / 2, opts.num, 0x2db74407b1ce6e93ULL));
                                 std::string value;
                                 const auto elapsed = fossil::bench::util::measure_time([&] {
                                     if(not backend.get(key, &value)) {
                                         throw std::runtime_error(
                                             "missing key during read-write");
                                     }
                                 });
                                 hist.add(elapsed.count());
                             } else {
                                 const auto write_index = op / 2;
                                 const auto key = static_cast<key_type>(
                                     random_index(write_index,
                                                  opts.num,
                                                  0x1f425a55d8b62f31ULL));
                                 const auto value = make_value(key, opts.value_size, 2);
                                 const auto elapsed = fossil::bench::util::measure_time(
                                     [&] { backend.put(key, std::move(value)); });
                                 hist.add(elapsed.count());
                             }
                             break;
                         }
                         }
                     }
                 });
}

template<typename Backend>
void run_backend_benchmarks(std::string_view backend_name, const options& opts, bool show_shards)
{
    print_header(backend_name, opts, show_shards);

    for(const auto& benchmark : parse_benchmarks(opts.benchmarks)) {
        Backend backend(opts);

        fossil::detail::rw_spin_lock::thread_counter.store(0, std::memory_order_relaxed);

        if(benchmark.id == benchmark_id::override_existing ||
           benchmark.id == benchmark_id::read_seq || benchmark.id == benchmark_id::read_random ||
           benchmark.id == benchmark_id::read_write) {
            prefill(backend, opts);
        }

        run_benchmark_case(backend, opts, benchmark);
    }
}

} // namespace fossil::bench::db_benchmark
