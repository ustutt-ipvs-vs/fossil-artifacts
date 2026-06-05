#include "db_benchmark_common.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <db.h>
#include <unistd.h>

namespace {

using namespace fossil::bench::db_benchmark;

auto is_retryable_bdb_error(int rc) -> bool
{
    return rc == DB_LOCK_DEADLOCK || rc == DB_LOCK_NOTGRANTED || rc == DB_REP_HANDLE_DEAD;
}

class bdb_backend
{
public:
    explicit bdb_backend(const options& opts)
    {
        try {
            env_home_ = make_benchmark_path(opts, "fossil_bdb_benchmark");
            std::filesystem::create_directories(env_home_);

            const int create_env_rc = db_env_create(&env_, 0);
            if(create_env_rc != 0) {
                throw std::runtime_error(db_strerror(create_env_rc));
            }

            const int deadlock_detect_rc = env_->set_lk_detect(env_, DB_LOCK_DEFAULT);
            if(deadlock_detect_rc != 0) {
                throw std::runtime_error(db_strerror(deadlock_detect_rc));
            }

            const int open_env_rc = env_->open(env_,
                                               env_home_.c_str(),
                                               DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG |
                                                   DB_INIT_MPOOL | DB_INIT_TXN | DB_THREAD,
                                               0600);
            if(open_env_rc != 0) {
                throw std::runtime_error(db_strerror(open_env_rc));
            }

            const int create_db_rc = db_create(&db_, env_, 0);
            if(create_db_rc != 0) {
                throw std::runtime_error(db_strerror(create_db_rc));
            }

            const int open_db_rc = db_->open(db_,
                                             nullptr,
                                             "benchmark.db",
                                             nullptr,
                                             DB_BTREE,
                                             DB_CREATE | DB_THREAD | DB_AUTO_COMMIT,
                                             0600);
            if(open_db_rc != 0) {
                throw std::runtime_error(db_strerror(open_db_rc));
            }
        } catch(...) {
            cleanup();
            throw;
        }
    }

    ~bdb_backend() { cleanup(); }

    void put(key_type key, std::string value)
    {
        auto key_bytes = encode_key(key);

        DBT key_dbt{};
        key_dbt.data = key_bytes.data();
        key_dbt.size = key_bytes.size();

        DBT value_dbt{};
        value_dbt.data = const_cast<char*>(value.data());
        value_dbt.size = value.size();

        while(true) {
            DB_TXN* txn = nullptr;
            const int begin_rc = env_->txn_begin(env_, nullptr, &txn, 0);
            if(begin_rc != 0) {
                throw std::runtime_error(db_strerror(begin_rc));
            }

            const int put_rc = db_->put(db_, txn, &key_dbt, &value_dbt, 0);
            if(is_retryable_bdb_error(put_rc)) {
                txn->abort(txn);
                continue;
            }
            if(put_rc != 0) {
                txn->abort(txn);
                throw std::runtime_error(db_strerror(put_rc));
            }

            const int commit_rc = txn->commit(txn, DB_TXN_WRITE_NOSYNC);
            if(is_retryable_bdb_error(commit_rc)) {
                continue;
            }
            if(commit_rc != 0) {
                throw std::runtime_error(db_strerror(commit_rc));
            }
            return;
        }
    }

    auto get(key_type key, std::string* value) -> bool
    {
        auto key_bytes = encode_key(key);

        DBT key_dbt{};
        key_dbt.data = key_bytes.data();
        key_dbt.size = key_bytes.size();

        DBT value_dbt{};
        value_dbt.flags = DB_DBT_MALLOC;

        const int get_rc = db_->get(db_, nullptr, &key_dbt, &value_dbt, 0);
        if(get_rc == DB_NOTFOUND) {
            return false;
        }
        if(get_rc != 0) {
            throw std::runtime_error(db_strerror(get_rc));
        }

        value->assign(static_cast<const char*>(value_dbt.data), value_dbt.size);
        std::free(value_dbt.data);
        return true;
    }

private:
    void cleanup() noexcept
    {
        if(db_ != nullptr) {
            db_->close(db_, 0);
            db_ = nullptr;
        }
        if(env_ != nullptr) {
            env_->close(env_, 0);
            env_ = nullptr;
        }
        if(not env_home_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(env_home_, error);
        }
    }

    DB_ENV* env_ = nullptr;
    DB* db_ = nullptr;
    std::filesystem::path env_home_;
};

} // namespace

auto main(int argc, const char* argv[]) -> int
{
    try {
        const auto opts = parse_cli(argc, argv, false);
        run_backend_benchmarks<bdb_backend>("Berkeley DB", opts, false);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr, "bdb benchmark failed: %s\n", error.what());
        return 1;
    }
}
