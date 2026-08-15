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

# spdlog: 优先系统安装版; 否则引入子模块 libs/spdlog
find_package(spdlog QUIET)
if(spdlog_FOUND)
  message(STATUS "spdlog: using system-installed spdlog (${spdlog_VERSION})")
  set(SPDLOG_LIBS spdlog::spdlog)
else()
  message(STATUS "spdlog: system spdlog not found, adding submodule libs/spdlog")
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/spdlog/CMakeLists.txt")
    message(FATAL_ERROR
      "spdlog not found in system and submodule not initialized at libs/spdlog.\n"
      "Please fetch it yourself:\n"
      "  git submodule update --init --recursive\n"
      "(or clone with --recurse-submodules, or install libspdlog-dev)")
  endif()
  add_subdirectory(libs/spdlog)
  set(SPDLOG_LIBS spdlog::spdlog)
endif()

# ----------------------------------------------------------------------------
# 链接期接管系统 malloc (独占堆模式)
#   ON  : 编入 src/override.cpp, 可执行目标加 -Wl,--wrap=malloc/free/calloc/realloc
#   OFF : 仅提供命名空间 API (wageco::), 依赖"独占堆"假设, 勿与系统 malloc 混用
# ----------------------------------------------------------------------------
option(MMEMORY_OVERRIDE_MALLOC "Link-time interpose system malloc via --wrap (exclusive heap)" OFF)
if(MMEMORY_OVERRIDE_MALLOC)
  message(STATUS "mmemory: overriding system malloc via --wrap (exclusive heap)")
  set(MMEMORY_OVERRIDE_SRC src/override.cpp)
else()
  message(STATUS "mmemory: namespaced API - exclusive-heap assumption, do NOT mix with system malloc")
endif()

add_library(mmemory STATIC src/mmemory.cpp src/allocator.cpp src/list.cpp src/log.cpp src/memory.cpp ${MMEMORY_OVERRIDE_SRC})
target_link_libraries(mmemory PUBLIC ${SPDLOG_LIBS})

# 编译成功后把对外公共头复制到构建目录 (build/include/mmemory.h),
# 方便外部项目引用; 内部头 (allocator.h 等) 属于实现细节, 不外发
add_custom_command(
  TARGET mmemory POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/include/mmemory.h
          ${CMAKE_BINARY_DIR}/include/mmemory.h
  COMMENT "Copying public header -> ${CMAKE_BINARY_DIR}/include/mmemory.h"
  VERBATIM)

# 单元测试: 同一源文件, 宏 MMEMORY_TEST_CUSTOM 切换命名空间 (进程级隔离)
#   CustomMemory:        定义宏 + 链接 mmemory (测 wageco 分配器)
#   CustomMemory_system: 不定义宏, 不链接 mmemory (测系统 malloc, 对照)
add_executable(${PROJECT_NAME} test/unittest_malloc.cpp)
target_compile_definitions(${PROJECT_NAME} PRIVATE MMEMORY_TEST_CUSTOM)
target_link_libraries(${PROJECT_NAME} mmemory ${GTEST_LIBS})

add_executable(${PROJECT_NAME}_system test/unittest_malloc.cpp)
target_link_libraries(${PROJECT_NAME}_system ${GTEST_LIBS})

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
  # 性能基准: 同一源文件, 宏 MMEMORY_TEST_CUSTOM 切换命名空间 (进程级隔离)
  #   CustomMemory_bench:        定义宏 + 链接 mmemory (测 wageco)
  #   CustomMemory_bench_system: 不定义宏, 不链接 mmemory (测系统 malloc)
  add_executable(${PROJECT_NAME}_bench test/bench_malloc.cpp)
  target_compile_definitions(${PROJECT_NAME}_bench PRIVATE MMEMORY_TEST_CUSTOM)
  target_link_libraries(${PROJECT_NAME}_bench mmemory benchmark::benchmark)

  add_executable(${PROJECT_NAME}_bench_system test/bench_malloc.cpp)
  target_link_libraries(${PROJECT_NAME}_bench_system benchmark::benchmark)
endif()

# 独占堆模式: 给可执行目标加 --wrap 链接选项 (链接期接管系统 malloc)
if(MMEMORY_OVERRIDE_MALLOC)
  target_link_options(${PROJECT_NAME} PRIVATE -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc)
  if(BENCHMARK_ENABLED)
    target_link_options(${PROJECT_NAME}_bench PRIVATE -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc)
  endif()
endif()

# ----------------------------------------------------------------------------
# 代码格式化: 检测到 clang-format 时提供 format / check-format 目标
#   用法: cmake --build build --target format        # 直接格式化源码
#         cmake --build build --target check-format  # 只检查, 不改文件 (CI)
# ----------------------------------------------------------------------------
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
  message(STATUS "clang-format: ${CLANG_FORMAT} (format/check-format targets enabled)")
  set(MMEMORY_FORMAT_FILES
    include/mmemory.h
    include/allocator.h
    include/block.h
    include/find_strategy.h
    include/internal.h
    include/list.h
    include/logging.h
    include/memory.h
    src/mmemory.cpp
    src/allocator.cpp
    src/list.cpp
    src/log.cpp
    src/memory.cpp
    src/override.cpp
    test/unittest_malloc.cpp
    test/bench_malloc.cpp)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT} -i --style=file ${MMEMORY_FORMAT_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Formatting sources with clang-format (see .clang-format)"
    VERBATIM)
  add_custom_target(check-format
    COMMAND ${CLANG_FORMAT} --dry-run --Werror --style=file ${MMEMORY_FORMAT_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking formatting with clang-format"
    VERBATIM)
else()
  message(STATUS "clang-format not found; format/check-format targets disabled")
endif()
