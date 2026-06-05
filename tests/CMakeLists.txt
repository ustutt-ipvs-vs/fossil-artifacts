add_executable(crash_recovery_test crash_recovery_test.cpp)
target_link_libraries(crash_recovery_test PRIVATE fossil)

add_test(NAME crash_recovery COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_crash_recovery_pmem.sh)
set_tests_properties(crash_recovery PROPERTIES TIMEOUT 120)
