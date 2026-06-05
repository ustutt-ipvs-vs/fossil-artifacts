#include "durabletx_compat.hpp"
#include "db_benchmark_common.hpp"

#define USE_TRINITY_VR_FC 1
#ifdef FOSSIL_DURABLETX_NVM
#define PWB_IS_CLWB 1
#define PM_USE_DAX 1
#else
#define PWB_IS_NOP 1
#endif
#define PM_FILE_NAME ::fossil::bench::db_benchmark::durabletx_pmem_file_name("/dev/shm/fossil_trinitydb_fc_benchmark_shared")

#include "../durabletx/ptms/trinity/TrinityVRFC.hpp"
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

#include "durabletx_ptmdb_benchmark.cpp"
#include "../durabletx/ptmdb/db_impl.cc"
#include "../durabletx/ptmdb/status.cc"
#include "../durabletx/ptmdb/port/port_posix.cc"
#include "../durabletx/ptmdb/util/histogram.cc"
#include "../durabletx/ptmdb/util/env_posix.cc"
#include "../durabletx/ptmdb/util/testutil.cc"
#include "../durabletx/ptmdb/write_batch.cc"
