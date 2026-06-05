#include "db_benchmark_common.hpp"

#include "db.h"
#include "db_impl.h"

#include <filesystem>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace {

using namespace fossil::bench::db_benchmark;

#if defined(USE_TRINITY_VR_FC)
constexpr std::string_view kBackendName = "TrinityDB (FC)";
#elif defined(USE_TRINITY_VR_TL2)
constexpr std::string_view kBackendName = "TrinityDB (TL2)";
#else
#error "durabletx_ptmdb_benchmark.cpp requires a Trinity compile definition"
#endif

auto make_ptmdb_key(key_type key) -> std::array<char, sizeof(key_type) + 1>
{
    std::array<char, sizeof(key_type) + 1> bytes{};
    const auto encoded = encode_key(key);
    std::copy(encoded.begin(), encoded.end(), bytes.begin());
    return bytes;
}

class durabletx_ptmdb_backend
{
public:
    explicit durabletx_ptmdb_backend(const options& opts)
    {
        try {
            path_ = make_benchmark_path(opts, "fossil_durabletx_ptmdb_benchmark");
            std::filesystem::create_directories(path_);

            ptmdb::Options open_options;
            open_options.create_if_missing = true;
            open_options.error_if_exists = true;

            const auto status = ptmdb::DB::Open(open_options, path_.string(), &db_);
            if(not status.ok()) {
                throw std::runtime_error(status.ToString());
            }
        } catch(...) {
            cleanup();
            throw;
        }
    }

    ~durabletx_ptmdb_backend() { cleanup(); }

    void put(key_type key, std::string value)
    {
        const auto key_bytes = make_ptmdb_key(key);
        const ptmdb::Slice key_slice(key_bytes.data(), sizeof(key_type));
        const ptmdb::Slice value_slice(value.data(), value.size());
        const auto status = db_->Put(ptmdb::WriteOptions(), key_slice, value_slice);

        if(not status.ok()) {
            throw std::runtime_error(status.ToString());
        }
    }

    auto get(key_type key, std::string* value) -> bool
    {
        const auto key_bytes = make_ptmdb_key(key);
        const ptmdb::Slice key_slice(key_bytes.data(), sizeof(key_type));
        const auto status = db_->Get(ptmdb::ReadOptions(), key_slice, value);

        if(status.IsNotFound()) {
            return false;
        }
        if(not status.ok()) {
            throw std::runtime_error(status.ToString());
        }
        return true;
    }

private:
    void cleanup() noexcept
    {
        if(db_ != nullptr) {
            auto* impl = static_cast<ptmdb::DBImpl*>(db_);
            PTM_UPDATE_TX([&] { PTM_PUT_ROOT(0, nullptr); });
            delete impl;
            db_ = nullptr;
        }

        std::error_code error;
        std::filesystem::remove(PM_FILE_NAME, error);
        if(not path_.empty()) {
            std::filesystem::remove_all(path_, error);
        }
    }

    ptmdb::DB* db_ = nullptr;
    std::filesystem::path path_;
};

} // namespace

auto main(int argc, const char* argv[]) -> int
{
    try {
        const auto opts = parse_cli(argc, argv, false);
        run_backend_benchmarks<durabletx_ptmdb_backend>(kBackendName, opts, false);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "%.*s benchmark failed: %s\n",
                     static_cast<int>(kBackendName.size()),
                     kBackendName.data(),
                     error.what());
        return 1;
    }
}
