cmake_minimum_required(VERSION 3.22)
project(fossil VERSION 0.1.0)

include(FetchContent)
include(cmake/flags.cmake)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

FetchContent_Declare(
  unordered_dense
  GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
  GIT_TAG v4.8.1
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(unordered_dense)

# Add the plugin source files
add_library(${PROJECT_NAME} INTERFACE
  # src/main.cpp
)

target_include_directories(${PROJECT_NAME} INTERFACE
  include/
)

target_link_libraries(${PROJECT_NAME} INTERFACE
  unordered_dense::unordered_dense
)

target_compile_options(${PROJECT_NAME} INTERFACE
  -mavx2
  -mclwb
)

# Add an executable that uses the plugin
add_executable(example examples/example.cpp)

target_link_libraries(example PRIVATE
  ${PROJECT_NAME}
)

set_sanitizers(example)


add_subdirectory(benchmarks)

enable_testing()
add_subdirectory(tests)
