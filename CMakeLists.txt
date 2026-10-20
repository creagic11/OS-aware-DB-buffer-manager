cmake_minimum_required(VERSION 3.20)
project(BufferScope VERSION 0.1.0 LANGUAGES CXX)

# C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Build type defaults
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# Include paths
include_directories(${PROJECT_SOURCE_DIR}/include)

# Core library
add_library(bufferscope_core
    src/page.cpp
    src/file_manager.cpp
    src/buffer_pool.cpp
    src/kv_store.cpp
)
target_include_directories(bufferscope_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# Main executable
add_executable(bufferscope src/main.cpp)
target_link_libraries(bufferscope bufferscope_core)

# Testing with GoogleTest
enable_testing()
find_package(GTest REQUIRED)

add_executable(page_test tests/page_test.cpp)
target_link_libraries(page_test bufferscope_core GTest::gtest_main)
add_test(NAME PageTest COMMAND page_test)

add_executable(file_manager_test tests/file_manager_test.cpp)
target_link_libraries(file_manager_test bufferscope_core GTest::gtest_main)
add_test(NAME FileManagerTest COMMAND file_manager_test)

add_executable(kv_correctness_test tests/kv_correctness_test.cpp)
target_link_libraries(kv_correctness_test bufferscope_core GTest::gtest_main)
add_test(NAME KVCorrectnessTest COMMAND kv_correctness_test)

add_executable(buffer_pool_test tests/buffer_pool_test.cpp)
target_link_libraries(buffer_pool_test bufferscope_core GTest::gtest_main)
add_test(NAME BufferPoolTest COMMAND buffer_pool_test)
