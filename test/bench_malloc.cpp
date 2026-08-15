// ============================================================================
// bench_malloc.cpp - 分配器性能基准 (单份代码, 宏切换命名空间)
// ----------------------------------------------------------------------------
// 编译宏 MMEMORY_TEST_CUSTOM 决定本文件测哪个分配器:
//   - 定义   → ALLOC/DEALLOC 指向 wageco:: (目标: CustomMemory_bench, 链 mmemory)
//   - 未定义 → ALLOC/DEALLOC 指向系统 ::malloc/free (目标: CustomMemory_bench_system)
// 两个目标从同一源文件编译, 生成两个独立可执行文件 —— 进程级隔离
// (本库独占堆, 与系统 malloc 互斥, 不能在同一个进程里同时测)。
// 六组基准 (前四组测 8 / 64 / 512 / 4096 字节; 后两组随机/稳态场景):
//   1. BM_AllocFree      单次 分配+释放 (小块频繁场景)
//   2. BM_Batch          批量: 先分配 256 块再全部释放 (含合并/回收压力)
//   3. BM_AllocFree_MT   多线程 (1/4 线程) 单次分配+释放 (锁竞争对比)
//   4. BM_RandomSize     随机大小 单次分配+释放 (混合大小池)
//   5. BM_AllocHold      分配保持: 维持 1024 块存活, 每轮替换一个 (稳态占用)
//   6. BM_Churn          随机分配/释放交替 (模拟真实负载)
// 用 --benchmark_filter 或输出文件名区分两份结果 (README 并排对比)。
// ============================================================================
#include <benchmark/benchmark.h>
#include <stdint.h>  // uint32_t (LCG 伪随机)
#include <stdlib.h>

#include <vector>

#ifdef MMEMORY_TEST_CUSTOM
#include "mmemory.h"
#define ALLOC(s) wageco::malloc(s)
#define DEALLOC(p) wageco::free(p)
#else
#define ALLOC(s) ::malloc(s)
#define DEALLOC(p) ::free(p)
#endif

namespace
{
const int kBatch = 256;  // 批量基准的单轮块数
}

// ---------- 1. 单次 分配 + 释放 ----------
// 系统 malloc: tcache 命中零系统调用; wageco: sbrk + 链表 + 锁
static void BM_AllocFree(benchmark::State& state)
{
    const size_t size = (size_t)state.range(0);
    for (auto _ : state)
    {
        void* p = ALLOC(size);
        benchmark::DoNotOptimize(p);  // 防止编译器把分配优化掉
        DEALLOC(p);
    }
}
BENCHMARK(BM_AllocFree)->Arg(8)->Arg(64)->Arg(512)->Arg(4096);

// ---------- 2. 批量: 先全部分配, 再全部释放 ----------
static void BM_Batch(benchmark::State& state)
{
    const size_t size = (size_t)state.range(0);
    std::vector<void*> ptrs(kBatch);
    for (auto _ : state)
    {
        for (int i = 0; i < kBatch; ++i) ptrs[i] = ALLOC(size);
        benchmark::DoNotOptimize(ptrs.data());
        for (int i = 0; i < kBatch; ++i) DEALLOC(ptrs[i]);
    }
}
BENCHMARK(BM_Batch)->Arg(8)->Arg(64)->Arg(512)->Arg(4096);

// ---------- 3. 多线程: 单次 分配 + 释放 ----------
// 系统 malloc 4 线程几乎无损失; wageco 全局锁导致串行化 (总耗时上升)
static void BM_AllocFree_MT(benchmark::State& state)
{
    const size_t size = (size_t)state.range(0);
    for (auto _ : state)
    {
        void* p = ALLOC(size);
        benchmark::DoNotOptimize(p);
        DEALLOC(p);
    }
}
BENCHMARK(BM_AllocFree_MT)->Arg(8)->Threads(1)->Threads(4);

// ---------- 4. 随机大小: 单次 分配 + 释放 (混合大小池) ----------
// 用 LCG 伪随机序列 (benchmark 循环内不能有慢的库调用)
static void BM_RandomSize(benchmark::State& state)
{
    uint32_t seed = 0x12345678u;
    for (auto _ : state)
    {
        seed = seed * 1664525u + 1013904223u;  // LCG
        size_t size = seed % 2000 + 1;
        void* p = ALLOC(size);
        benchmark::DoNotOptimize(p);
        DEALLOC(p);
    }
}
BENCHMARK(BM_RandomSize);

// ---------- 5. 分配保持: 维持 N 块存活, 每轮替换一个 (稳态占用) ----------
// 模拟长期运行的内存占用场景; 用静态数组存指针, 避免测试框架引入系统分配
static void BM_AllocHold(benchmark::State& state)
{
    const size_t size = (size_t)state.range(0);
    const int kHold = 1024;  // 保持 1024 块
    static void* held[kHold];
    for (int i = 0; i < kHold; ++i) held[i] = ALLOC(size);
    size_t idx = 0;
    for (auto _ : state)
    {
        DEALLOC(held[idx]);
        held[idx] = ALLOC(size);
        idx = (idx + 1) % kHold;
    }
    for (int i = 0; i < kHold; ++i) DEALLOC(held[i]);
}
BENCHMARK(BM_AllocHold)->Arg(64)->Arg(1024);

// ---------- 6. Churn: 随机分配/释放交替 (模拟真实负载) ----------
static void BM_Churn(benchmark::State& state)
{
    const int kPool = 4096;
    static void* pool[kPool];
    static size_t sizes[kPool];
    size_t live = 0;
    uint32_t seed = 0x9e3779b9u;
    for (auto _ : state)
    {
        seed = seed * 1664525u + 1013904223u;
        if ((seed & 1u) && live < kPool)
        {
            size_t sz = seed % 2000 + 1;
            pool[live] = ALLOC(sz);
            sizes[live] = sz;
            ++live;
        }
        else if (live > 0)
        {
            size_t idx = seed % live;
            DEALLOC(pool[idx]);
            pool[idx] = pool[live - 1];
            sizes[idx] = sizes[live - 1];
            --live;
        }
    }
    for (size_t i = 0; i < live; ++i) DEALLOC(pool[i]);
}
BENCHMARK(BM_Churn);

BENCHMARK_MAIN();
