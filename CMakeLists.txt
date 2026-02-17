cmake_minimum_required(VERSION 3.28)
project(MemoryMapper VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Policies ---
cmake_policy(SET CMP0167 NEW)

# --- Sanitizer presets ---
include(cmake/Sanitizers.cmake)

# --- Dependencies ---
find_package(Boost 1.83 REQUIRED CONFIG)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(GTest REQUIRED)
find_package(benchmark REQUIRED)

# --- Main library (allocator + tracker + server) ---
add_library(memory_mapper_lib STATIC
    src/allocator/arena.cpp
    src/allocator/free_list.cpp
    src/tracker/tracker.cpp
    src/server/ws_server.cpp
    src/interface/visualization_arena.cpp
    src/interface/cache_analyzer.cpp
    src/allocator/tracked_resource.cpp
)

target_include_directories(memory_mapper_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(memory_mapper_lib PUBLIC
    Boost::headers
    nlohmann_json::nlohmann_json
)

# --- Main executable ---
add_executable(memory_mapper src/main.cpp)
target_link_libraries(memory_mapper PRIVATE memory_mapper_lib)

# --- Server simulation executable ---
add_executable(server_sim
    src/simulation/sim_main.cpp
    src/simulation/server_sim.cpp
    src/simulation/request_generator.cpp
)
target_link_libraries(server_sim PRIVATE memory_mapper_lib)

# --- Tests ---
enable_testing()

add_executable(memory_mapper_tests
    tests/test_arena.cpp
    tests/test_free_list.cpp
    tests/test_tracker.cpp
    tests/test_visualization_arena.cpp
    tests/test_cache_analyzer.cpp
)

target_link_libraries(memory_mapper_tests PRIVATE
    memory_mapper_lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(memory_mapper_tests)

add_executable(stress_test_arena
    tests/stress_test_arena.cpp
)
target_link_libraries(stress_test_arena PRIVATE
    memory_mapper_lib
)

# --- Benchmarks ---
add_executable(memory_mapper_bench
    bench/bench_allocator.cpp
)

target_link_libraries(memory_mapper_bench PRIVATE
    memory_mapper_lib
    benchmark::benchmark
    benchmark::benchmark_main
)

add_executable(memory_mapper_bench_throughput
    bench/bench_throughput.cpp
)

target_link_libraries(memory_mapper_bench_throughput PRIVATE
    memory_mapper_lib
    benchmark::benchmark
    benchmark::benchmark_main
)

add_executable(memory_mapper_bench_scalability
    bench/bench_scalability.cpp
)

target_link_libraries(memory_mapper_bench_scalability PRIVATE
    memory_mapper_lib
    benchmark::benchmark
    benchmark::benchmark_main
)

add_executable(memory_mapper_bench_contention
    bench/bench_contention.cpp
)

target_link_libraries(memory_mapper_bench_contention PRIVATE
    memory_mapper_lib
    benchmark::benchmark
)

add_executable(memory_mapper_bench_serialization
    bench/bench_serialization.cpp
)

target_link_libraries(memory_mapper_bench_serialization PRIVATE
    memory_mapper_lib
    nlohmann_json::nlohmann_json
    benchmark::benchmark
)

# --- Copy web assets to build directory ---
add_custom_command(TARGET memory_mapper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/web
        $<TARGET_FILE_DIR:memory_mapper>/web
    COMMENT "Copying web assets to build directory"
)
