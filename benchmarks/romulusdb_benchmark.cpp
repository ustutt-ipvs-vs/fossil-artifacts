#include "db_benchmark_common.hpp"

#include "db.h"
#include "db_impl.h"

#include <filesystem>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace {

using namespace fossil::bench::db_benchmark;

constexpr std::string_view kBackendName = "RomulusDB";

class romulusdb_backend
{
public:
    explicit romulusdb_backend(const options& opts)
    {
        try {
            path_ = make_benchmark_path(opts, "fossil_romulusdb_benchmark");
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

    ~romulusdb_backend() { cleanup(); }

    void put(key_type key, std::string value)
    {
        const auto key_bytes = encode_key(key);
        const ptmdb::Slice key_slice(key_bytes.data(), key_bytes.size());
        const ptmdb::Slice value_slice(value.data(), value.size());
        const auto status = db_->Put(ptmdb::WriteOptions(), key_slice, value_slice);

        if(not status.ok()) {
            throw std::runtime_error(status.ToString());
        }
    }

    auto get(key_type key, std::string* value) -> bool
    {
        const auto key_bytes = encode_key(key);
        const ptmdb::Slice key_slice(key_bytes.data(), key_bytes.size());
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
        delete static_cast<ptmdb::DBImpl*>(db_);
        db_ = nullptr;

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
        run_backend_benchmarks<romulusdb_backend>(kBackendName, opts, false);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "romulusdb benchmark failed: %s\n", error.what());
        return 1;
    }
}
