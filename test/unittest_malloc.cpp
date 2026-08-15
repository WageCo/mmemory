// ============================================================================
// unittest_malloc.cpp - 分配器单元测试 (单份代码, 宏切换命名空间)
// ----------------------------------------------------------------------------
// 编译宏 MMEMORY_TEST_CUSTOM 决定本文件测哪个分配器:
//   - 定义   → ALLOC/DEALLOC 指向 wageco:: (目标: CustomMemory, 链接 mmemory)
//   - 未定义 → ALLOC/DEALLOC 指向系统 ::malloc/free (目标: CustomMemory_system)
// 两个目标从同一源文件编译, 生成两个独立可执行文件 —— 进程级隔离
// (本库独占堆, 与系统 malloc 互斥, 不能在同一进程混测)。
// 注意: 部分回归测试 (double free / 堆回收 / 查找策略) 依赖本库行为,
// 系统 malloc 下 double free 是未定义行为, 因此这些用例仅在有宏时编译。
// 构建由 CMake 完成 (见 CMakeLists.txt), 不需要手动编译。
// ============================================================================
#include <gtest/gtest.h>
#include <stdint.h>  // uintptr_t, SIZE_MAX
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  // sbrk (FragmentationShrinkTest)

#ifdef MMEMORY_TEST_CUSTOM
#include "internal.h"  // BestFitTest 需要 (构造独立分配器)
#include "mmemory.h"
#define ALLOC(s) wageco::malloc(s)
#define DEALLOC(p) wageco::free(p)
#define CALLOC(n, s) wageco::calloc(n, s)
#define REALLOC(p, s) wageco::realloc(p, s)
#else
#include <stdlib.h>
#define ALLOC(s) ::malloc(s)
#define DEALLOC(p) ::free(p)
#define CALLOC(n, s) ::calloc(n, s)
#define REALLOC(p, s) ::realloc(p, s)
#endif

// 基础压力测试: 同模式大量分配/释放 (q/m 常驻, 中间循环同大小反复分配释放)
// 宏切换: 定义时测 wageco, 未定义时测系统 malloc —— 同一份代码两种对照
TEST(testMalloc, StressTest)
{
    const size_t count = INT16_MAX * 100;
    size_t size = std::rand() % 1000;
    void* q = ALLOC(size);
    void* m = ALLOC(size);
    DEALLOC(q);
    for (int i = 0; i < count; ++i)
    {
        void* p = ALLOC(size);
        if (p) DEALLOC(p);
    }
    DEALLOC(m);
}

// 对齐校验: 返回指针必须 16 字节对齐 (多种大小)
TEST(testMalloc, AlignmentTest)
{
    const size_t sizes[] = {1, 8, 15, 16, 17, 100, 4096, 65537};
    for (size_t sz : sizes)
    {
        void* p = ALLOC(sz);
        ASSERT_NE(p, nullptr) << "alloc failed, size=" << sz;
        EXPECT_EQ((reinterpret_cast<uintptr_t>(p) & 15u), 0u) << "not 16-aligned, size=" << sz;
        DEALLOC(p);
    }
}

// 边界大小: malloc(0) / 1B / 大块 (1MB)
TEST(testMalloc, BoundarySizeTest)
{
    // malloc(0): 允许返回 NULL 或最小块, 但释放必须安全
    void* z = ALLOC(0);
    if (z) DEALLOC(z);

    // 1 字节
    void* p1 = ALLOC(1);
    ASSERT_NE(p1, nullptr);
    *((char*)p1) = 0x5A;
    EXPECT_EQ(*((char*)p1), 0x5A);
    DEALLOC(p1);

    // 大块 (1MB)
    void* big = ALLOC(1 << 20);
    ASSERT_NE(big, nullptr);
    memset(big, 0xAA, 1 << 20);
    DEALLOC(big);
}

// calloc: 清零 + 乘法溢出返回 NULL
TEST(testMalloc, CallocTest)
{
    void* p = CALLOC(10, 100);
    ASSERT_NE(p, nullptr);
    const unsigned char* b = (const unsigned char*)p;
    for (int i = 0; i < 1000; ++i) EXPECT_EQ(b[i], 0) << "byte " << i;
    DEALLOC(p);

    // 溢出: num*size 超过 SIZE_MAX 应返回 NULL (SIZE_MAX/2+1 与 2 的乘积溢出)
    void* q = CALLOC(SIZE_MAX / 2 + 1, 2);
    EXPECT_EQ(q, nullptr);
}

// 相邻块数据完好性: 多块写不同模式, 互不覆盖
TEST(testMalloc, DataIntegrityTest)
{
    const int N = 64;
    void* ptrs[N];
    size_t sizes[N];
    for (int i = 0; i < N; ++i)
    {
        sizes[i] = (size_t)((i % 5 + 1) * 32);  // 32/64/96/128/160
        ptrs[i] = ALLOC(sizes[i]);
        ASSERT_NE(ptrs[i], nullptr);
        memset(ptrs[i], (int)(i + 1), sizes[i]);  // 每块不同填充模式
    }
    for (int i = 0; i < N; ++i)
    {
        const unsigned char* b = (const unsigned char*)ptrs[i];
        for (size_t j = 0; j < sizes[i]; ++j)
        {
            EXPECT_EQ(b[j], (unsigned char)(i + 1)) << "block " << i << " byte " << j;
        }
    }
    for (int i = 0; i < N; ++i) DEALLOC(ptrs[i]);
}

// 随机 churn: 分配/释放混合交替, 释放时校验数据完好
// (模拟真实负载的分配-释放模式)
TEST(testMalloc, ChurnTest)
{
    const int kMax = 5000;
    void* ptrs[kMax];
    size_t sizes[kMax];
    unsigned char values[kMax];  // 每块的填充值 (释放时按记录校验)
    int live = 0;
    for (int iter = 0; iter < 20000; ++iter)
    {
        if (live < kMax && (std::rand() % 2 == 0))
        {
            size_t sz = std::rand() % 2000 + 1;
            void* p = ALLOC(sz);
            ASSERT_NE(p, nullptr);
            const unsigned char v = (unsigned char)(live % 250 + 1);
            memset(p, v, sz);
            ptrs[live] = p;
            sizes[live] = sz;
            values[live] = v;
            ++live;
        }
        else if (live > 0)
        {
            int idx = std::rand() % live;
            // 释放前校验数据完好 (整块抽查, 提前退出)
            const unsigned char v = values[idx];
            const unsigned char* b = (const unsigned char*)ptrs[idx];
            bool ok = true;
            for (size_t j = 0; j < sizes[idx]; ++j)
            {
                if (b[j] != v)
                {
                    ok = false;
                    break;
                }
            }
            EXPECT_TRUE(ok) << "data corrupted, iter=" << iter;
            DEALLOC(ptrs[idx]);
            ptrs[idx] = ptrs[live - 1];
            sizes[idx] = sizes[live - 1];
            values[idx] = values[live - 1];
            --live;
        }
    }
    for (int i = 0; i < live; ++i) DEALLOC(ptrs[i]);
}

#ifdef MMEMORY_TEST_CUSTOM
// ============================================================================
// 以下回归测试依赖本库行为 (系统 malloc 下 double free 是 UB, 不参与对照)
// ============================================================================

// 回归: realloc 修复验证
//   - 64B 块扩容到 4096B 后, 前 64 字节内容必须完整保留
//     (修复前 memcpy 按新大小拷贝会越界读, 破坏 header 之后的堆内容);
//   - realloc(ptr, 0) 必须释放旧块并返回 NULL (修复前泄漏旧块)。
TEST(testMalloc, ReallocTest)
{
    void* p = ALLOC(64);
    ASSERT_NE(p, nullptr);
    memset(p, 0xAB, 64);

    void* q = REALLOC(p, 4096);
    ASSERT_NE(q, nullptr);
    const unsigned char* b = (const unsigned char*)q;
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_EQ(b[i], 0xAB) << "byte " << i;
    }
    memset(q, 0, 4096);

    EXPECT_EQ(REALLOC(q, 0), nullptr);
}

// 回归: double free 防御
//   free 同一指针两次: 第二次应被检测 (inuse 标志 + 地址校验) 并忽略,
//   不崩溃、不破坏链表; 同时输出 error 级别日志。
TEST(testMalloc, DoubleFreeTest)
{
    void* p = ALLOC(128);
    ASSERT_NE(p, nullptr);
    DEALLOC(p);
    EXPECT_NO_FATAL_FAILURE(DEALLOC(p));
    void* q = ALLOC(128);
    ASSERT_NE(q, nullptr);
    DEALLOC(q);
}

// 回归: 碎片回收验证
//   分配 20000 块随机大小, 乱序释放全部, 断言 program break 基本回到原值。
//   修复前 (无合并): 乱序释放的块滞留空闲链表, 堆无法回收, 该断言必挂;
//   修复后 (合并 + 反向释放): 全部回收, 断言通过。
TEST(testMalloc, FragmentationShrinkTest)
{
    const int N = 20000;
    void* ptrs[N];
    char* brk0 = (char*)sbrk(0);
    for (int i = 0; i < N; ++i)
    {
        size_t sz = std::rand() % 2000 + 1;
        void* p = ALLOC(sz);
        ASSERT_NE(p, nullptr) << "malloc failed at i=" << i;
        ptrs[i] = p;
        memset(p, 0x5A, sz);  // 确认内存真实可用
    }
    // 乱序释放, 制造碎片
    for (int i = 0; i < N; ++i)
    {
        size_t j = std::rand() % N;
        void* tmp = ptrs[i];
        ptrs[i] = ptrs[j];
        ptrs[j] = tmp;
    }
    for (int i = 0; i < N; ++i)
    {
        DEALLOC(ptrs[i]);
    }
    char* brk1 = (char*)sbrk(0);
    // 全部释放后堆应基本回收 (< 4KB 容差, 修复前会残留数百 KB 甚至更多)
    EXPECT_LT((size_t)(brk1 - brk0), 4096);
}

// BestFit 查找策略冒烟测试: 验证策略可注入且分配/释放行为正确
// (使用独立构造的 Allocator + BestFit, 与全局 first-fit 分配器互不干扰)
TEST(testMalloc, BestFitTest)
{
    wageco::HeaderList free_list(wageco::block_size_of);
    wageco::SbrkMemory memory;
    wageco::BestFit strategy;
    wageco::Allocator alloc(&free_list, &memory, &strategy);

    void* a = alloc.malloc(100);
    ASSERT_NE(a, nullptr);
    void* b = alloc.malloc(200);
    ASSERT_NE(b, nullptr);
    memset(a, 0xAB, 100);
    memset(b, 0xCD, 200);
    // 数据完好性
    EXPECT_EQ(memcmp(a, std::string(100, (char)0xAB).c_str(), 100), 0);
    EXPECT_EQ(memcmp(b, std::string(200, (char)0xCD).c_str(), 200), 0);
    // 释放后再次分配应能复用空闲块
    alloc.free(a);
    alloc.free(b);
    void* c = alloc.malloc(150);
    ASSERT_NE(c, nullptr);
    memset(c, 0x5A, 150);
    alloc.free(c);
}
#endif  // MMEMORY_TEST_CUSTOM
