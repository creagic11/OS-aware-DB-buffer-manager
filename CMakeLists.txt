cmake_minimum_required(VERSION 3.20)
project(BufferScope VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUFFERSCOPE_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
option(BUFFERSCOPE_WERROR "Treat warnings as errors" ON)

add_compile_options(-Wall -Wextra -Wpedantic)
if(BUFFERSCOPE_WERROR)
    add_compile_options(-Werror)
endif()
if(BUFFERSCOPE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# ── Core library ──────────────────────────────────────────────────────
add_library(bufferscope_core
    src/page.cpp
    src/file_manager.cpp
    src/clock_replacer.cpp
    src/buffer_pool.cpp
    src/kv_store.cpp
    src/wal.cpp
    src/recovery.cpp
    src/crash.cpp
)
target_include_directories(bufferscope_core PUBLIC ${PROJECT_SOURCE_DIR}/include)

add_executable(bufferscope src/main.cpp)
target_link_libraries(bufferscope bufferscope_core)

# ── Benchmark harness ─────────────────────────────────────────────────
add_library(bufferscope_bench bench/workload.cpp)
target_include_directories(bufferscope_bench PUBLIC ${PROJECT_SOURCE_DIR}/bench)
target_link_libraries(bufferscope_bench bufferscope_core)

add_executable(bench_runner bench/runner.cpp)
target_link_libraries(bench_runner bufferscope_bench bufferscope_core)

# Google Benchmark microbenchmarks (optional: skipped if the package is absent)
find_package(benchmark QUIET)
if(benchmark_FOUND)
    add_executable(micro_bench bench/micro_bench.cpp)
    target_link_libraries(micro_bench bufferscope_core benchmark::benchmark)
else()
    message(STATUS "Google Benchmark not found: micro_bench target disabled")
endif()

# ── Tests ─────────────────────────────────────────────────────────────
enable_testing()
find_package(GTest REQUIRED)
include(GoogleTest)

function(bufferscope_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} bufferscope_core GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

bufferscope_test(page_test                  tests/page_test.cpp)
bufferscope_test(file_manager_test          tests/file_manager_test.cpp)
bufferscope_test(kv_correctness_test        tests/kv_correctness_test.cpp)
bufferscope_test(buffer_pool_test           tests/buffer_pool_test.cpp)
bufferscope_test(buffer_pool_reference_test tests/buffer_pool_reference_test.cpp)
bufferscope_test(clock_replacer_test        tests/clock_replacer_test.cpp)
bufferscope_test(buffer_pool_eviction_test  tests/buffer_pool_eviction_test.cpp)
bufferscope_test(direct_io_test             tests/direct_io_test.cpp)
bufferscope_test(io_mode_correctness_test   tests/io_mode_correctness_test.cpp)
bufferscope_test(wal_recovery_test          tests/wal_recovery_test.cpp)
bufferscope_test(crash_injection_test       tests/crash_injection_test.cpp)
