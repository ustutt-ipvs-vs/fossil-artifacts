#include "db_benchmark_common.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

#include <leveldb/db.h>
#include <unistd.h>

namespace {

using namespace fossil::bench::db_benchmark;

class leveldb_backend
{
public:
    explicit leveldb_backend(const options& opts)
    {
        try {
            path_ = make_benchmark_path(opts, "fossil_leveldb_benchmark");
            std::filesystem::create_directories(path_);

            leveldb::Options open_options;
            open_options.create_if_missing = true;
            open_options.error_if_exists = true;

            const auto status = leveldb::DB::Open(open_options, path_.string(), &db_);
            if(not status.ok()) {
                throw std::runtime_error(status.ToString());
            }
        } catch(...) {
            cleanup();
            throw;
        }
    }

    ~leveldb_backend() { cleanup(); }

    void put(key_type key, std::string value)
    {
        const auto key_bytes = encode_key(key);
        const leveldb::Slice key_slice(key_bytes.data(), key_bytes.size());
        const leveldb::Slice value_slice(value.data(), value.size());
        const auto status = db_->Put(leveldb::WriteOptions(), key_slice, value_slice);

        if(not status.ok()) {
            throw std::runtime_error(status.ToString());
        }
    }

    auto get(key_type key, std::string* value) -> bool
    {
        const auto key_bytes = encode_key(key);
        const leveldb::Slice key_slice(key_bytes.data(), key_bytes.size());
        const auto status = db_->Get(leveldb::ReadOptions(), key_slice, value);

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
        delete db_;
        db_ = nullptr;
        if(not path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    leveldb::DB* db_ = nullptr;
    std::filesystem::path path_;
};

} // namespace

auto main(int argc, const char* argv[]) -> int
{
    try {
        const auto opts = parse_cli(argc, argv, false);
        run_backend_benchmarks<leveldb_backend>("LevelDB", opts, false);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "leveldb benchmark failed: %s\n", error.what());
        return 1;
    }
}
