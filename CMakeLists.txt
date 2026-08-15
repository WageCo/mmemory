# ============================================================================
# MMEMORY - 教学用简易内存分配器
# ============================================================================
cmake_minimum_required(VERSION 3.20.0)
project(CustomMemory LANGUAGES CXX)

# ----------------------------------------------------------------------------
# 全局编译选项
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 默认 Release, 否则 benchmark 在无优化下失真
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 仅支持 Linux (分配器依赖 sbrk / pthread)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  message(STATUS "Current OS: ${CMAKE_SYSTEM_NAME}")
else()
  message(FATAL_ERROR "Current OS: ${CMAKE_SYSTEM_NAME}, only Linux is supported")
endif()

# ----------------------------------------------------------------------------
# googletest: 优先系统安装版; 否则引入子模块 libs/googletest
# (CMake 不负责拉取子模块, 未初始化时给出提示, 请手动 git submodule update --init)
# ----------------------------------------------------------------------------
find_package(GTest QUIET)
if(GTest_FOUND)
  message(STATUS "gtest: using system-installed googletest (${GTest_VERSION})")
  set(GTEST_LIBS GTest::gtest_main GTest::gtest)
else()
  message(STATUS "gtest: system googletest not found, adding submodule libs/googletest")
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/googletest/CMakeLists.txt")
    message(FATAL_ERROR
      "googletest not found in system and submodule not initialized at libs/googletest.\n"
      "Please fetch it yourself:\n"
      "  git submodule update --init --recursive\n"
      "(or clone with --recurse-submodules, or install libgtest-dev)")
  endif()
  add_subdirectory(libs/googletest)
  set(GTEST_LIBS gtest_main gtest)
endif()

# ----------------------------------------------------------------------------
# 分配器静态库 + 单元测试
# ----------------------------------------------------------------------------
include_directories(include)

add_library(mmemory STATIC src/mmemory.cpp)

add_executable(${PROJECT_NAME} test/unittest_malloc.cpp)
target_link_libraries(${PROJECT_NAME} mmemory ${GTEST_LIBS})

# ----------------------------------------------------------------------------
# benchmark: 优先系统安装版; 否则引入子模块 libs/benchmark
# 子模块未初始化时提示用户手动拉取, 并跳过基准目标 (不阻塞测试构建)
# ----------------------------------------------------------------------------
set(BENCHMARK_ENABLED FALSE)
find_package(benchmark QUIET)
if(benchmark_FOUND)
  message(STATUS "benchmark: using system-installed benchmark")
  set(BENCHMARK_ENABLED TRUE)
else()
  message(STATUS "benchmark: system benchmark not found, adding submodule libs/benchmark")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/benchmark/CMakeLists.txt")
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Build benchmark tests" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "Build benchmark gtest tests" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "Install benchmark" FORCE)
    add_subdirectory(libs/benchmark EXCLUDE_FROM_ALL)
    set(BENCHMARK_ENABLED TRUE)
  else()
    message(WARNING
      "benchmark not found in system and submodule not initialized at libs/benchmark.\n"
      "Benchmark target skipped. To enable, fetch the submodule yourself:\n"
      "  git submodule update --init --recursive\n"
      "(or install libbenchmark-dev)")
  endif()
endif()

if(BENCHMARK_ENABLED)
  add_executable(${PROJECT_NAME}_bench test/bench_malloc.cpp)
  target_link_libraries(${PROJECT_NAME}_bench mmemory benchmark::benchmark)
endif()
