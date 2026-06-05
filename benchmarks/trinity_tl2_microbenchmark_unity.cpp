#include "db_benchmark_common.hpp"
#include "durabletx_compat.hpp"

#define USE_TRINITY_VR_TL2 1
#ifdef FOSSIL_DURABLETX_NVM
#define PWB_IS_CLWB 1
#define PM_USE_DAX 1
#else
#define PWB_IS_NOP 1
#endif
#define PM_FILE_NAME ::fossil::bench::db_benchmark::durabletx_pmem_file_name("/dev/shm/fossil_trinity_tl2_microbenchmark_shared")

namespace trinityvrtl2 {
struct OpData;
[[noreturn]] void abortTx(OpData* myd);
} // namespace trinityvrtl2

#include "../durabletx/ptms/trinity/TrinityVRTL2.hpp"

#define FOSSIL_DURABLETX_BACKEND_NAME "TrinityVR-TL2"
#define FOSSIL_DURABLETX_TM trinityvrtl2::Trinity
#define FOSSIL_DURABLETX_PERSIST trinityvrtl2::persist

#include "durabletx_microbenchmark.cpp"
