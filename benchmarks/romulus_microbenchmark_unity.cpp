#include "db_benchmark_common.hpp"
#include "durabletx_compat.hpp"

#define USE_ROMULUS_LOG 1
#ifdef FOSSIL_DURABLETX_NVM
#define PWB_IS_CLWB 1
#define PM_USE_DAX 1
#else
#define PWB_IS_NOP 1
#endif
#define PM_FILE_NAME ::fossil::bench::db_benchmark::durabletx_pmem_file_name("/dev/shm/fossil_romulus_microbenchmark_shared")

#include "../durabletx/ptmdb/otherdb/redodb/ptms/romuluslog/RomulusLog.hpp"

#define FOSSIL_DURABLETX_BACKEND_NAME "RomulusLog"
#define FOSSIL_DURABLETX_PERSIST romuluslog::persist
#define FOSSIL_DURABLETX_ROMULUS 1

namespace fossil::bench::durabletx_microbenchmark {

struct romulus_benchmark_tm
{
    static auto className() -> std::string { return romuluslog::RomulusLog::className(); }

    template<typename F>
    static void updateTx(F&& func)
    {
        romuluslog::RomulusLog::write_transaction(std::forward<F>(func));
    }

    template<typename R, typename F>
    static auto updateTx(F&& func) -> R
    {
        romuluslog::RomulusLog::write_transaction(std::forward<F>(func));
        return R{};
    }

    template<typename F>
    static void readTx(F&& func)
    {
        romuluslog::RomulusLog::readTx(std::forward<F>(func));
    }

    template<typename R, typename F>
    static auto readTx(F&& func) -> R
    {
        romuluslog::RomulusLog::readTx(std::forward<F>(func));
        return R{};
    }

    template<typename T, typename... Args>
    static auto tmNew(Args&&... args) -> T*
    {
        auto* storage = romuluslog::RomulusLog::pmalloc(sizeof(T));
        return new (storage) T(std::forward<Args>(args)...);
    }

    template<typename T>
    static void tmDelete(T* object)
    {
        if(object == nullptr) {
            return;
        }

        object->~T();
        romuluslog::RomulusLog::pfree(object);
    }

    static auto pmalloc(std::size_t size) -> void* { return romuluslog::RomulusLog::pmalloc(size); }
    static void pfree(void* object) { romuluslog::RomulusLog::pfree(object); }

    template<typename T>
    static auto get_object(int idx) -> T*
    {
        return romuluslog::RomulusLog::template get_object<T>(idx);
    }

    template<typename T>
    static void put_object(int idx, T* object)
    {
        romuluslog::RomulusLog::template put_object<T>(idx, object);
    }
};

} // namespace fossil::bench::durabletx_microbenchmark

#define FOSSIL_DURABLETX_TM ::fossil::bench::durabletx_microbenchmark::romulus_benchmark_tm

#include "durabletx_microbenchmark.cpp"
#include "../durabletx/ptmdb/otherdb/redodb/ptms/romuluslog/RomulusLog.cpp"
#include "../durabletx/ptmdb/otherdb/redodb/ptms/romuluslog/malloc.cpp"
#include "../durabletx/ptmdb/otherdb/redodb/common/ThreadRegistry.cpp"
