#include "durabletx_compat.hpp"

#define USE_ROMULUS_LOG 1
#ifdef FOSSIL_DURABLETX_NVM
#define PWB_IS_CLWB 1
#define PM_USE_DAX 1
#else
#define PWB_IS_NOP 1
#endif
#define PM_FILE_NAME ::fossil::bench::db_benchmark::durabletx_pmem_file_name("/dev/shm/fossil_romulusdb_benchmark_shared")

#include "romulusdb_benchmark.cpp"
#include "../durabletx/ptmdb/otherdb/redodb/db_impl.cc"
#include "../durabletx/ptmdb/otherdb/redodb/status.cc"
#include "../durabletx/ptmdb/otherdb/redodb/port/port_posix.cc"
#include "../durabletx/ptmdb/otherdb/redodb/util/histogram.cc"
#include "../durabletx/ptmdb/otherdb/redodb/util/env_posix.cc"
#include "../durabletx/ptmdb/otherdb/redodb/util/testutil.cc"
#include "../durabletx/ptmdb/otherdb/redodb/write_batch.cc"
#include "../durabletx/ptmdb/otherdb/redodb/common/ThreadRegistry.cpp"
#include "../durabletx/ptmdb/otherdb/redodb/ptms/romuluslog/RomulusLog.cpp"
#include "../durabletx/ptmdb/otherdb/redodb/ptms/romuluslog/malloc.cpp"
