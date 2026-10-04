cmake_minimum_required(VERSION 3.20)
project(NeoGraphConcurrentBenchmark LANGUAGES C CXX)

# Reuse NeoGraph's public target and its pinned SDK acquisition. This consumer
# must not manually assemble static archives or copy dependency include paths.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." neograph EXCLUDE_FROM_ALL)
add_executable(bench_ng bench_concurrent_neograph.cpp)
target_link_libraries(bench_ng PRIVATE neograph::core)
