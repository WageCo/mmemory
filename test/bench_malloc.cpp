// ============================================================================
// bench_malloc.cpp - Google Benchmark 对比基准
// ----------------------------------------------------------------------------
// 目的: 定量对比 系统 malloc 与 wageco 自定义分配器 的性能差异,
//       展示"为什么真实分配器需要 tcache / per-thread arena / 无锁设计"。
//
// 四组基准:
//   1. BM_*_MallocFree   单次 分配+释放 (小块频繁场景)
//   2. BM_*_Batch        批量: 先分配 256 块再全部释放 (含合并/回收压力)
//   3. BM_*_MallocFree_MT 多线程 (1/4 线程) 单次分配+释放 (锁竞争对比)
//
// 每个基准测 4 种大小: 8 / 64 / 512 / 4096 字节。
//
// 构建: CMake 自动创建 CustomMemory_bench 目标
//   (需要系统装有 google/benchmark, 或子模块 test/benchmark 可拉取)。
// ============================================================================
#include <benchmark/benchmark.h>
#include <stdlib.h>
#include <vector>

#include "mmemory.h"

namespace
{
const int kBatch = 256; // 批量基准的单轮块数
}

// ---------- 1. 单次 分配 + 释放 ----------
// 系统 malloc: tcache 命中时零系统调用, 极快
static void BM_System_MallocFree(benchmark::State &state)
{
    const size_t size = (size_t)state.range(0);
    for (auto _ : state)
    {
        void *p = malloc(size);
        benchmark::DoNotOptimize(p); // 防止编译器把分配优化掉
        free(p);
    }
}
BENCHMARK(BM_System_MallocFree)->Arg(8)->Arg(64)->Arg(512)->Arg(4096);

// wageco: 每次 sbrk 系统调用 + 链表操作 + 全局锁, 慢在固定开销
static void BM_Custom_MallocFree(benchmark::State &state)
{
    const size_t size = (size_t)state.range(0);
    for (auto _ : state)
    {
        void *p = wageco::malloc(size);
        benchmark::DoNotOptimize(p);
        wageco::free(p);
    }
}
BENCHMARK(BM_Custom_MallocFree)->Arg(8)->Arg(64)->Arg(512)->Arg(4096);

// ---------- 2. 批量: 先全部分配, 再全部释放 ----------
// 大块场景下系统 malloc 走 mmap/大块路径, 双方成本接近, 差距缩小
static void BM_System_Batch(benchmark::State &state)
{
    const size_t size = (size_t)state.range(0);
    std::vector<void *> ptrs(kBatch);
    for (auto _ : state)
    {
        for (int i = 0; i < kBatch; ++i)
            ptrs[i] = malloc(size);
        benchmark::DoNotOptimize(ptrs.data());
        for (int i = 0; i < kBatch; ++i)
            free(ptrs[i]);
    }
}
BENCHMARK(BM_System_Batch)->Arg(8)->Arg(64)->Arg(512)->Arg(4096);

static void BM_Custom_Batch(benchmark::State &state)
{
    const size_t size = (size_t)state.range(0);
    std::vector<void *> ptrs(kBatch);
    for (auto _ : state)
    {
        for (int i = 0; i < kBatch; ++i)
            ptrs[i] = wageco::malloc(size);
        benchmark::DoNotOptimize(ptrs.data());
        for (int i = 0; i < kBatch; ++i)
            wageco::free(ptrs[i]);
    }
}
BENCHMARK(BM_Custom_Batch)->Arg(8)->Arg(64)->Arg(512)->Arg(4096);

// ---------- 3. 多线程: 单次 分配 + 释放 ----------
// 系统 malloc 4 线程几乎无损失; wageco 全局锁导致 4 线程总耗时反而上升 (串行化)
static void BM_System_MallocFree_MT(benchmark::State &state)
{
    const size_t size = (size_t)state.range(0);
    for (auto _ : state)
    {
        void *p = malloc(size);
        benchmark::DoNotOptimize(p);
        free(p);
    }
}
BENCHMARK(BM_System_MallocFree_MT)->Arg(8)->Threads(1)->Threads(4);

static void BM_Custom_MallocFree_MT(benchmark::State &state)
{
    const size_t size = (size_t)state.range(0);
    for (auto _ : state)
    {
        void *p = wageco::malloc(size);
        benchmark::DoNotOptimize(p);
        wageco::free(p);
    }
}
BENCHMARK(BM_Custom_MallocFree_MT)->Arg(8)->Threads(1)->Threads(4);

BENCHMARK_MAIN();
