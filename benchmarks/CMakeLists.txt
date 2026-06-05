set(FOSSIL_HAS_PMDK_BENCHMARKS OFF CACHE BOOL "Enable PMDK-backed benchmarks" FORCE)

find_path(PMEMOBJ_INCLUDE_DIR NAMES libpmemobj.h)
find_library(PMEMOBJ_LIBRARY NAMES pmemobj)

if(PMEMOBJ_INCLUDE_DIR AND PMEMOBJ_LIBRARY)
  add_library(fossil_benchmark_pmdk INTERFACE)
  target_include_directories(fossil_benchmark_pmdk INTERFACE ${PMEMOBJ_INCLUDE_DIR})
  target_link_libraries(fossil_benchmark_pmdk INTERFACE ${PMEMOBJ_LIBRARY})
  set(FOSSIL_HAS_PMDK_BENCHMARKS ON CACHE BOOL "Enable PMDK-backed benchmarks" FORCE)
endif()

add_executable(vector_microbenchmark vector_microbenchmark.cpp)
target_link_libraries(vector_microbenchmark PRIVATE fossil)
if(FOSSIL_HAS_PMDK_BENCHMARKS)
  target_link_libraries(vector_microbenchmark PRIVATE fossil_benchmark_pmdk)
  target_compile_definitions(vector_microbenchmark PRIVATE FOSSIL_HAS_PMDK=1)
endif()

add_executable(map_microbenchmark map_microbenchmark.cpp)
target_link_libraries(map_microbenchmark PRIVATE fossil)

add_executable(rbtree_microbenchmark rbtree_microbenchmark.cpp)
target_link_libraries(rbtree_microbenchmark PRIVATE fossil)

add_executable(
  romulus_microbenchmark
  romulus_microbenchmark_unity.cpp
)
target_include_directories(
  romulus_microbenchmark
  PRIVATE
    ../durabletx
    ../durabletx/ptms
    ../durabletx/ptmdb/otherdb/redodb/ptms
    ../durabletx/ptmdb/otherdb/redodb
    ../include/
)

add_executable(
  romulus_microbenchmark_nvm
  romulus_microbenchmark_unity.cpp
)
target_compile_definitions(romulus_microbenchmark_nvm PRIVATE FOSSIL_DURABLETX_NVM=1)
target_include_directories(
  romulus_microbenchmark_nvm
  PRIVATE
    ../durabletx
    ../durabletx/ptms
    ../durabletx/ptmdb/otherdb/redodb/ptms
    ../durabletx/ptmdb/otherdb/redodb
    ../include/
)

add_executable(
  trinity_fc_microbenchmark
  trinity_fc_microbenchmark_unity.cpp
)
target_include_directories(
  trinity_fc_microbenchmark
  PRIVATE
    ../durabletx
    ../durabletx/ptmdb/otherdb/redodb
    ../include/
)

add_executable(
  trinity_fc_microbenchmark_nvm
  trinity_fc_microbenchmark_unity.cpp
)
target_compile_definitions(trinity_fc_microbenchmark_nvm PRIVATE FOSSIL_DURABLETX_NVM=1)
target_include_directories(
  trinity_fc_microbenchmark_nvm
  PRIVATE
    ../durabletx
    ../durabletx/ptmdb/otherdb/redodb
    ../include/
)

add_executable(
  trinity_tl2_microbenchmark
  trinity_tl2_microbenchmark_unity.cpp
)
target_include_directories(
  trinity_tl2_microbenchmark
  PRIVATE
    ../durabletx
    ../durabletx/ptmdb/otherdb/redodb
    ../include/
)

add_executable(
  trinity_tl2_microbenchmark_nvm
  trinity_tl2_microbenchmark_unity.cpp
)
target_compile_definitions(trinity_tl2_microbenchmark_nvm PRIVATE FOSSIL_DURABLETX_NVM=1)
target_include_directories(
  trinity_tl2_microbenchmark_nvm
  PRIVATE
    ../durabletx
    ../durabletx/ptmdb/otherdb/redodb
    ../include/
)

add_executable(toggle_map_benchmark toggle_map_benchmark.cpp)
target_link_libraries(toggle_map_benchmark PRIVATE fossil leveldb db)
if(FOSSIL_HAS_PMDK_BENCHMARKS)
  target_link_libraries(toggle_map_benchmark PRIVATE fossil_benchmark_pmdk)
  target_compile_definitions(toggle_map_benchmark PRIVATE FOSSIL_HAS_PMDK=1)
endif()

add_executable(leveldb_benchmark leveldb_benchmark.cpp)
target_link_libraries(leveldb_benchmark PRIVATE fossil)

add_executable(leveldb_benchmark_sharded leveldb_benchmark_sharded.cpp)
target_link_libraries(leveldb_benchmark_sharded PRIVATE fossil)

add_executable(leveldb_benchmark_baseline leveldb_benchmark_baseline.cpp)
target_link_libraries(leveldb_benchmark_baseline PRIVATE fossil)

add_executable(leveldb_db_benchmark leveldb_db_benchmark.cpp)
target_link_libraries(leveldb_db_benchmark PRIVATE leveldb fossil)

add_executable(bdb_benchmark bdb_benchmark.cpp)
target_link_libraries(bdb_benchmark PRIVATE db fossil)

add_executable(fossildb_benchmark fossildb_benchmark.cpp)
target_link_libraries(fossildb_benchmark PRIVATE fossil)
# set_sanitizers(fossildb_benchmark)

add_executable(unordered_map_rwlock_benchmark unordered_map_rwlock_benchmark.cpp)
target_link_libraries(unordered_map_rwlock_benchmark PRIVATE fossil)


add_executable(
  trinitydb_fc_benchmark
  trinitydb_fc_benchmark_unity.cpp
)
target_include_directories(
  trinitydb_fc_benchmark
  PRIVATE
    ../durabletx/ptmdb
    ../durabletx/ptmdb/util
    ../durabletx/ptmdb/port
    ../include/
)

add_executable(
  trinitydb_fc_benchmark_nvm
  trinitydb_fc_benchmark_unity.cpp
)
target_compile_definitions(trinitydb_fc_benchmark_nvm PRIVATE FOSSIL_DURABLETX_NVM=1)
target_include_directories(
  trinitydb_fc_benchmark_nvm
  PRIVATE
    ../durabletx/ptmdb
    ../durabletx/ptmdb/util
    ../durabletx/ptmdb/port
    ../include/
)

add_executable(
  trinitydb_tl2_benchmark
  trinitydb_tl2_benchmark_unity.cpp
)
target_include_directories(
  trinitydb_tl2_benchmark
  PRIVATE
    ../durabletx/ptmdb
    ../durabletx/ptmdb/util
    ../durabletx/ptmdb/port
    ../include/
)

add_executable(
  trinitydb_tl2_benchmark_nvm
  trinitydb_tl2_benchmark_unity.cpp
)
target_compile_definitions(trinitydb_tl2_benchmark_nvm PRIVATE FOSSIL_DURABLETX_NVM=1)
target_include_directories(
  trinitydb_tl2_benchmark_nvm
  PRIVATE
    ../durabletx/ptmdb
    ../durabletx/ptmdb/util
    ../durabletx/ptmdb/port
    ../include/
)

add_executable(
  romulusdb_benchmark
  romulusdb_benchmark_unity.cpp
)
target_include_directories(
  romulusdb_benchmark
  PRIVATE
    ../durabletx/ptmdb/otherdb/redodb
    ../durabletx/ptmdb/otherdb/redodb/util
    ../durabletx/ptmdb/otherdb/redodb/port
    ../include/
)

add_executable(
  romulusdb_benchmark_nvm
  romulusdb_benchmark_unity.cpp
)
target_compile_definitions(romulusdb_benchmark_nvm PRIVATE FOSSIL_DURABLETX_NVM=1)
target_include_directories(
  romulusdb_benchmark_nvm
  PRIVATE
    ../durabletx/ptmdb/otherdb/redodb
    ../durabletx/ptmdb/otherdb/redodb/util
    ../durabletx/ptmdb/otherdb/redodb/port
    ../include/
)
