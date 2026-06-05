#include "durabletx_compat.hpp"
#include "db_benchmark_common.hpp"

#define USE_TRINITY_VR_TL2 1
#ifdef FOSSIL_DURABLETX_NVM
#define PWB_IS_CLWB 1
#define PM_USE_DAX 1
#else
#define PWB_IS_NOP 1
#endif
#define PM_FILE_NAME ::fossil::bench::db_benchmark::durabletx_pmem_file_name("/dev/shm/fossil_trinitydb_tl2_benchmark_shared")

#define INCLUDED_FROM_MULTIPLE_CPP
#include "../durabletx/ptms/trinity/TrinityVRTL2.hpp"
#include "../durabletx/ptms/ptm.h"

namespace fossil::bench::db_benchmark {

inline auto durabletx_ptmdb_key_compare(const char* lhs, const char* rhs, std::size_t count)
    -> int
{
    return std::memcmp(lhs, rhs, count);
}

} // namespace fossil::bench::db_benchmark

#undef PTM_STRCMP
#define PTM_STRCMP ::fossil::bench::db_benchmark::durabletx_ptmdb_key_compare

namespace trinityvrtl2 {

ThreadRegistry gThreadRegistry {};
std::atomic<uint64_t>* gHashLock {nullptr};
alignas(128) std::atomic<uint64_t> gClockPaddingA {0};
alignas(128) std::atomic<uint64_t> gClock {1};
alignas(128) std::atomic<uint64_t> gClockPaddingB {0};
Trinity gTrinity {};
thread_local OpData* tl_opdata {nullptr};
thread_local ThreadCheckInCheckOut tl_tcico {};

void thread_registry_deregister_thread(const int tid)
{
    gThreadRegistry.deregister_thread(tid);
}

void abortTx(OpData* myd)
{
    myd->writeSet.rollbackVR(myd->tid);
    uint64_t nextClock = gClock.fetch_add(1) + 1;
    myd->writeSet.unlock(nextClock, myd->tid);
    myd->numAborts++;
    std::longjmp(myd->env, 1);
}

} // namespace trinityvrtl2

#include "durabletx_ptmdb_benchmark.cpp"
#include "../durabletx/ptmdb/db_impl.cc"
#include "../durabletx/ptmdb/status.cc"
#include "../durabletx/ptmdb/port/port_posix.cc"
#include "../durabletx/ptmdb/util/histogram.cc"
#include "../durabletx/ptmdb/util/env_posix.cc"
#include "../durabletx/ptmdb/util/testutil.cc"
#include "../durabletx/ptmdb/write_batch.cc"
