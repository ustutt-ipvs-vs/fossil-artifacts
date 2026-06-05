#include "db_benchmark_common.hpp"
#include "durabletx_compat.hpp"

#define USE_TRINITY_VR_FC 1
#ifdef FOSSIL_DURABLETX_NVM
#define PWB_IS_CLWB 1
#define PM_USE_DAX 1
#else
#define PWB_IS_NOP 1
#endif
#define PM_FILE_NAME ::fossil::bench::db_benchmark::durabletx_pmem_file_name("/dev/shm/fossil_trinity_fc_microbenchmark_shared")

#include "../durabletx/ptms/trinity/TrinityVRFC.hpp"

#define FOSSIL_DURABLETX_BACKEND_NAME "TrinityVR-FC"
#define FOSSIL_DURABLETX_TM trinityvrfc::Trinity
#define FOSSIL_DURABLETX_PERSIST trinityvrfc::persist

#include "durabletx_microbenchmark.cpp"
