// ============================================================================
// unittest_malloc.cpp - gtest 单元测试
// ----------------------------------------------------------------------------
// 覆盖两类用例:
//   1. 对照实验: 同样模式的分配/释放, 系统 malloc 与 wageco::malloc 各跑一遍
//      (MallocTest / MyMallocTest);
//   2. 回归测试: 针对历史上修过的 bug
//      - ReallocTest:          realloc 越界读 / realloc(ptr, 0) 泄漏;
//      - FragmentationShrinkTest: 乱序释放后堆能否完整回收 (碎片合并)。
// 构建由 CMake 完成 (见 CMakeLists.txt), 不需要手动编译。
// ============================================================================
#include <gtest/gtest.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mmemory.h"

// 迭代次数: 3,276,700 次分配/释放 (INT16_MAX * 100)
const size_t const_test = INT16_MAX * 100;

// 对照: 系统 malloc 的分配/释放模式 (q/m 常驻, 中间循环同大小反复 malloc/free)
void test_malloc(size_t count)
{
    size_t size = std::rand() % 1000;
    void *q = malloc(size);
    void *m = malloc(size);
    free(q);
    for (int i = 0; i < count; ++i)
    {
        void *p = malloc(size);
        if (p)
            free(p);
    }
    free(m);
}

// 对照: wageco::malloc 跑同样的模式
void test_my_malloc(size_t count)
{
    size_t size = std::rand() % 1000;
    void *q = wageco::malloc(size);
    void *m = wageco::malloc(size);
    wageco::free(q);
    for (int i = 0; i < count; ++i)
    {
        void *p = wageco::malloc(size);
        if (p)
            wageco::free(p);
    }
    wageco::free(m);
}

// 系统 malloc 基础压力测试
TEST(testMalloc, MallocTest)
{
    test_malloc(const_test);
}

// 自定义分配器基础压力测试
TEST(testMalloc, MyMallocTest)
{
    test_my_malloc(const_test);
}

// 回归: realloc 修复验证
//   - 64B 块扩容到 4096B 后, 前 64 字节内容必须完整保留
//     (修复前 memcpy 按新大小拷贝会越界读, 破坏 header 之后的堆内容);
//   - realloc(ptr, 0) 必须释放旧块并返回 NULL (修复前泄漏旧块)。
TEST(testMalloc, ReallocTest)
{
    void *p = wageco::malloc(64);
    ASSERT_NE(p, nullptr);
    memset(p, 0xAB, 64);

    void *q = wageco::realloc(p, 4096);
    ASSERT_NE(q, nullptr);
    const unsigned char *b = (const unsigned char *)q;
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_EQ(b[i], 0xAB) << "byte " << i;
    }
    memset(q, 0, 4096);

    EXPECT_EQ(wageco::realloc(q, 0), nullptr);
}

// 回归: 碎片回收验证
//   分配 20000 块随机大小, 乱序释放全部, 断言 program break 基本回到原值。
//   修复前 (无合并/精确匹配): 乱序释放的块滞留空闲链表, 堆无法回收, 该断言必挂;
//   修复后 (first-fit + 分割 + 合并 + 堆顶连带回收): 全部回收, 断言通过。
TEST(testMalloc, FragmentationShrinkTest)
{
    const int N = 20000;
    void *ptrs[N];
    char *brk0 = (char *)sbrk(0);
    for (int i = 0; i < N; ++i)
    {
        size_t sz = std::rand() % 2000 + 1;
        void *p = wageco::malloc(sz);
        ASSERT_NE(p, nullptr) << "malloc failed at i=" << i;
        ptrs[i] = p;
        memset(p, 0x5A, sz); // 确认内存真实可用
    }
    // 乱序释放, 制造碎片 (修复前: 无合并, 堆无法回收)
    for (int i = 0; i < N; ++i)
    {
        size_t j = std::rand() % N;
        void *tmp = ptrs[i];
        ptrs[i] = ptrs[j];
        ptrs[j] = tmp;
    }
    for (int i = 0; i < N; ++i)
    {
        wageco::free(ptrs[i]);
    }
    char *brk1 = (char *)sbrk(0);
    // 全部释放后堆应基本回收 (< 4KB 容差, 修复前会残留数百 KB 甚至更多)
    EXPECT_LT((size_t)(brk1 - brk0), 4096);
}
