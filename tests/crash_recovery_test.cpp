#include <fossil/detail/nv_allocator.hpp>
#include <fossil/detail/pmap.hpp>
#include <fossil/detail/pvec.hpp>
#include <fossil/transaction.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using pvec_type = fossil::detail::pvec<std::uint64_t>;
using pmap_type = fossil::detail::pmap<std::uint64_t, std::uint64_t>;

constexpr auto kMapKeys = std::uint64_t{256};
constexpr auto kNestedKeys = std::uint64_t{64};
constexpr auto kAllocatorShard = std::size_t{1000};
constexpr auto kWorkerSteps = std::size_t{1'000'000};
constexpr auto kDefaultRuns = std::size_t{24};

struct refs
{
    std::uint64_t vec = 0;
    std::uint64_t map = 0;
    std::uint64_t nested_index = 0;
};

[[nodiscard]] auto map_value(std::uint64_t key) -> std::uint64_t
{
    return (key * 0x9E3779B97F4A7C15ULL) ^ 0xD1B54A32D192ED03ULL;
}

[[nodiscard]] auto refs_file(const std::filesystem::path& dir) -> std::filesystem::path
{
    return dir / "refs.txt";
}

void write_refs(const std::filesystem::path& dir, refs value)
{
    std::ofstream out(refs_file(dir));
    if(not out) {
        throw std::runtime_error("failed to write refs file");
    }
    out << value.vec << ' ' << value.map << ' ' << value.nested_index << '\n';
}

[[nodiscard]] auto read_refs(const std::filesystem::path& dir) -> refs
{
    std::ifstream in(refs_file(dir));
    refs value{};
    if((not in) or not(in >> value.vec >> value.map >> value.nested_index)) {
        throw std::runtime_error("failed to read refs file");
    }
    return value;
}

void configure_storage(const std::filesystem::path& dir)
{
    std::filesystem::create_directories(dir);
    fossil::detail::nv_allocator::set_storage_directory(dir);
    fossil::detail::nv_allocator::set_dax_mapping(true);
}

void allocator_burst(std::uint64_t seed)
{
    auto alloc = fossil::detail::nv_allocator::create_unsafe(kAllocatorShard);
    auto recovered = alloc.recover();

    for(std::size_t i = 0; i < std::min<std::size_t>(recovered.size(), 8); ++i) {
        alloc.free(recovered[i]);
    }

    std::mt19937_64 rng(seed);
    std::vector<fossil::detail::nv_span> live;
    live.reserve(128);

    for(std::size_t i = 0; i < 128; ++i) {
        const auto size = std::size_t{32} + static_cast<std::size_t>(rng() % 4096);
        auto span = alloc.allocate(size);
        std::fill(span.begin(), span.end(), static_cast<std::byte>(i & 0xffU));

        if((rng() & 1U) == 0U) {
            alloc.free(span);
        } else {
            live.push_back(span);
        }
    }

    for(std::size_t i = 0; i < live.size() / 2; ++i) {
        alloc.free(live[i]);
    }
    alloc.flush();
}

void init_mode(const std::filesystem::path& dir)
{
    configure_storage(dir);

    auto& repo = fossil::object_repo();
    auto vec = repo.create<pvec_type>();
    auto map = repo.create<pmap_type>(kMapKeys);
    auto nested_index = repo.create<pmap_type>(kNestedKeys);

    fossil::transaction(vec, [](pvec_type& value) {
        for(std::uint64_t i = 0; i < 32; ++i) {
            value.emplace_back(i);
        }
    });

    fossil::transaction(map, [](pmap_type& value) {
        for(std::uint64_t key = 0; key < kMapKeys / 2; ++key) {
            value.put(key, map_value(key));
        }
    });

    write_refs(dir, refs{.vec = vec.id, .map = map.id, .nested_index = nested_index.id});
    allocator_burst(1);
}

void vector_commit(fossil::reference<pvec_type>& vec, std::mt19937_64& rng, bool large)
{
    fossil::transaction(vec, [&](pvec_type& value) {
        if(value.size() > 16'384) {
            for(std::size_t i = 0; i < 512 && value.size() > 32; ++i) {
                value.pop_back();
            }
        }

        const auto count = large ? std::size_t{2048}
                                 : std::size_t{1 + static_cast<std::size_t>(rng() % 16)};
        for(std::size_t i = 0; i < count; ++i) {
            const auto index = static_cast<std::uint64_t>(value.size());
            value.emplace_back(index);
        }
    });
}

void map_commit(fossil::reference<pmap_type>& map, std::mt19937_64& rng)
{
    fossil::transaction(map, [&](pmap_type& value) {
        for(std::size_t i = 0; i < 64; ++i) {
            const auto key = rng() % kMapKeys;
            if((rng() % 5) == 0) {
                value.remove(key);
            } else {
                value.put(key, map_value(key));
            }
        }
    });
}

void nested_commit(fossil::reference<pmap_type>& nested_index, std::mt19937_64& rng)
{
    fossil::transaction(nested_index, [&](pmap_type& value) {
        const auto key = rng() % kNestedKeys;

        auto nested = fossil::object_repo().create<pvec_type>();
        value.put(key, nested.id);
    });
}

void worker_mode(const std::filesystem::path& dir, std::uint64_t seed, std::size_t steps)
{
    configure_storage(dir);
    const auto ids = read_refs(dir);

    fossil::reference<pvec_type> vec{ids.vec};
    fossil::reference<pmap_type> map{ids.map};
    fossil::reference<pmap_type> nested_index{ids.nested_index};
    std::mt19937_64 rng(seed);

    for(std::size_t step = 0; step < steps; ++step) {
        switch(rng() % 5) {
        case 0: vector_commit(vec, rng, false); break;
        case 1: map_commit(map, rng); break;
        case 2: vector_commit(vec, rng, true); break;
        case 3: nested_commit(nested_index, rng); break;
        case 4: allocator_burst(seed ^ step); break;
        default: std::unreachable();
        }
    }
}

[[nodiscard]] auto verify_vector(fossil::reference<pvec_type>& vec, std::string& error) -> bool
{
    return fossil::transaction(vec, [&](const pvec_type& value) {
        if(value.size() > 1'000'000) {
            error = "vector size is implausibly large";
            return false;
        }

        for(std::size_t i = 0; i < value.size(); ++i) {
            if(value[i] != i) {
                error = "vector index invariant failed at " + std::to_string(i);
                return false;
            }
        }
        return true;
    });
}

[[nodiscard]] auto verify_map(fossil::reference<pmap_type>& map, std::string& error) -> bool
{
    return fossil::transaction(map, [&](const pmap_type& value) {
        std::size_t present = 0;
        for(std::uint64_t key = 0; key < kMapKeys; ++key) {
            const auto* entry = value.get(key);
            if(entry == nullptr) {
                continue;
            }
            ++present;
            if(*entry != map_value(key)) {
                error = "map value invariant failed for key " + std::to_string(key);
                return false;
            }
        }

        if(present != value.size()) {
            error = "map size does not match countable key set";
            return false;
        }
        return true;
    });
}

[[nodiscard]] auto verify_allocator(std::string& error) -> bool
{
    auto alloc = fossil::detail::nv_allocator::create_unsafe(kAllocatorShard);
    auto recovered = alloc.recover();
    std::ranges::sort(recovered, {}, &fossil::detail::nv_span::offset);

    for(std::size_t i = 1; i < recovered.size(); ++i) {
        if(recovered[i - 1].offset() == recovered[i].offset()) {
            error = "allocator recovered duplicate live offsets";
            return false;
        }
    }

    auto span = alloc.allocate(257);
    if(span.size() != 257) {
        error = "allocator returned an unexpected span size";
        return false;
    }
    alloc.free(span);
    return true;
}

[[nodiscard]] auto verify_nested_refs(fossil::reference<pmap_type>& nested_index,
                                      std::string& error) -> bool
{
    std::vector<std::pair<std::uint64_t, std::uint64_t>> nested_refs;
    const auto index_ok = fossil::transaction(nested_index, [&](const pmap_type& value) {
        for(std::uint64_t key = 0; key < kNestedKeys; ++key) {
            if(const auto* ref_id = value.get(key); ref_id != nullptr) {
                nested_refs.emplace_back(key, *ref_id);
            }
        }

        if(nested_refs.size() != value.size()) {
            error = "nested index size does not match countable key set";
            return false;
        }
        return true;
    });

    if(not index_ok) {
        return false;
    }

    for(const auto& [key, ref_id] : nested_refs) {
        fossil::reference<pvec_type> nested{ref_id};
        const auto nested_ok = fossil::transaction(nested, [&](const pvec_type& value) {
            if(value.size() != 0) {
                error = "nested object unexpectedly contains payload for key " +
                    std::to_string(key);
                return false;
            }
            return true;
        });

        if(not nested_ok) {
            return false;
        }
    }

    return true;
}

void verify_mode(const std::filesystem::path& dir)
{
    configure_storage(dir);
    const auto ids = read_refs(dir);

    fossil::reference<pvec_type> vec{ids.vec};
    fossil::reference<pmap_type> map{ids.map};
    fossil::reference<pmap_type> nested_index{ids.nested_index};

    std::string error;
    if(not verify_vector(vec, error) or not verify_map(map, error) or
       not verify_nested_refs(nested_index, error) or not verify_allocator(error)) {
        throw std::runtime_error(error);
    }
}

[[nodiscard]] auto spawn_child(const std::filesystem::path& executable,
                               const std::vector<std::string>& args) -> pid_t
{
    const auto pid = fork();
    if(pid < 0) {
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }

    if(pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        const auto executable_string = executable.string();
        argv.push_back(const_cast<char*>(executable_string.c_str()));
        for(const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execv(executable_string.c_str(), argv.data());
        _exit(127);
    }

    return pid;
}

[[nodiscard]] auto wait_for(pid_t pid) -> int
{
    int status = 0;
    while(waitpid(pid, &status, 0) < 0) {
        if(errno != EINTR) {
            throw std::runtime_error(std::string("waitpid failed: ") + std::strerror(errno));
        }
    }
    return status;
}

void require_success(const std::filesystem::path& executable, const std::vector<std::string>& args)
{
    const auto status = wait_for(spawn_child(executable, args));
    if(not(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        throw std::runtime_error("child process failed");
    }
}

[[nodiscard]] auto kill_after(const std::filesystem::path& executable,
                              const std::vector<std::string>& args,
                              std::chrono::microseconds delay) -> int
{
    const auto pid = spawn_child(executable, args);
    std::this_thread::sleep_for(delay);
    if(kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        throw std::runtime_error(std::string("kill failed: ") + std::strerror(errno));
    }
    return wait_for(pid);
}

void orchestrator_mode(const std::filesystem::path& executable,
                       const std::filesystem::path& nvm_dir,
                       std::size_t runs)
{
    if(runs == 0) {
        throw std::runtime_error("--runs must be greater than zero");
    }

    std::filesystem::create_directories(nvm_dir);
    if(not std::filesystem::is_directory(nvm_dir)) {
        throw std::runtime_error("--nvram-dir must name a directory");
    }

    const auto dir = nvm_dir /
        ("fossil-crash-recovery-" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    struct cleanup
    {
        std::filesystem::path dir;
        ~cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        }
    } cleanup_guard{dir};

    require_success(executable, {"--init", dir.string()});
    require_success(executable, {"--verify", dir.string()});

    std::mt19937_64 rng(0xC0FFEE);
    std::size_t worker_kills = 0;
    std::size_t recovery_kills = 0;

    for(std::size_t trial = 0; trial < runs; ++trial) {
        const auto worker_delay =
            std::chrono::microseconds(500 + static_cast<int>(rng() % 12'000));
        const auto worker_status = kill_after(executable,
                                              {"--worker",
                                               dir.string(),
                                               std::to_string(rng()),
                                               std::to_string(kWorkerSteps)},
                                              worker_delay);
        if(WIFSIGNALED(worker_status) && WTERMSIG(worker_status) == SIGKILL) {
            ++worker_kills;
        }

        require_success(executable, {"--verify", dir.string()});

        if((trial % 4) == 0) {
            const auto recovery_delay =
                std::chrono::microseconds(100 + static_cast<int>(rng() % 1500));
            const auto recovery_status =
                kill_after(executable, {"--verify", dir.string()}, recovery_delay);
            if(WIFSIGNALED(recovery_status) && WTERMSIG(recovery_status) == SIGKILL) {
                ++recovery_kills;
            }
            require_success(executable, {"--verify", dir.string()});
        }
    }

    std::cout << "crash_recovery: PASS worker_kills=" << worker_kills
              << " recovery_kills=" << recovery_kills
              << " invariants=vector,map,nested,allocator\n";
}

[[nodiscard]] auto arg(std::span<char*> args, std::size_t index) -> std::string_view
{
    if(index >= args.size()) {
        throw std::runtime_error("missing command line argument");
    }
    return args[index];
}

} // namespace

auto main(int argc, char** argv) -> int
{
    try {
        auto args = std::span<char*>{argv, static_cast<std::size_t>(argc)};
        if(argc == 1) {
            throw std::runtime_error("missing --nvram-dir <nvram-directory> [--runs N]");
        }

        const auto mode = arg(args, 1);
        if(mode == "--nvram-dir" || mode == "--runs") {
            std::optional<std::filesystem::path> nvm_dir;
            auto runs = kDefaultRuns;

            for(std::size_t i = 1; i < args.size(); ++i) {
                const auto option = arg(args, i);
                if(option == "--nvram-dir") {
                    nvm_dir = std::filesystem::path{arg(args, ++i)};
                } else if(option == "--runs") {
                    runs = static_cast<std::size_t>(std::stoull(std::string{arg(args, ++i)}));
                } else {
                    throw std::runtime_error("unknown orchestrator option");
                }
            }

            if(not nvm_dir.has_value()) {
                throw std::runtime_error("missing --nvram-dir <nvram-directory>");
            }

            orchestrator_mode(std::filesystem::absolute(argv[0]), *nvm_dir, runs);
        } else if(mode == "--init") {
            init_mode(std::filesystem::path{arg(args, 2)});
        } else if(mode == "--verify") {
            verify_mode(std::filesystem::path{arg(args, 2)});
        } else if(mode == "--worker") {
            const auto steps = argc > 4 ? static_cast<std::size_t>(std::stoull(argv[4]))
                                        : kWorkerSteps;
            worker_mode(std::filesystem::path{arg(args, 2)}, std::stoull(argv[3]), steps);
        } else {
            throw std::runtime_error("unknown mode");
        }
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "crash_recovery_test: " << e.what() << '\n';
        return 1;
    }
}
