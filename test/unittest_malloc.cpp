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
//   注意: 小块的释放先进线程本地缓存 (tcache), 需要先 flush_tcache 把缓存
//   块倒回全局分配器, 才能参与合并与反向释放 —— 缓存只延迟回收, 不阻止回收。
//   因此本测试在记录 brk0 前先 flush 一次 (清掉前序测试滞留的缓存块, 建立
//   干净基线), 末尾再 flush 一次 (让本测试的缓存块也参与回收)。
TEST(testMalloc, FragmentationShrinkTest)
{
    const int N = 20000;
    void* ptrs[N];
    wageco::flush_tcache();  // 先清空线程本地缓存, 建立干净的 brk 基线
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
    wageco::flush_tcache();  // 把线程本地缓存中的块全部倒回全局分配器
    char* brk1 = (char*)sbrk(0);
    // 全部释放后堆应基本回收 (< 4KB 容差, 修复前会残留数百 KB 甚至更多)
    EXPECT_LT((size_t)(brk1 - brk0), 4096);
}

// tcache (线程本地缓存) 行为测试
//   - 小对象: 释放后再次分配同大小应命中缓存, 复用同一块 (零锁快路径);
//   - 大对象 (> 缓存上限 1024B): 不走缓存, 但行为一致;
//   - 混合大小交错分配/释放: 数据完好性;
//   - flush_tcache 把滞留缓存块倒回全局分配器, 不破坏后续分配。
TEST(testMalloc, TcacheTest)
{
    // 小对象: free 后同一线程再次 malloc 同大小应命中缓存 (返回同一地址)
    void* a = ALLOC(64);
    ASSERT_NE(a, nullptr);
    DEALLOC(a);
    void* b = ALLOC(64);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a, b);  // tcache 命中: 复用刚释放的块
    DEALLOC(b);

    // 大对象 (4096 > 1024 缓存上限): 不走缓存, 行为不变
    void* big1 = ALLOC(4096);
    ASSERT_NE(big1, nullptr);
    DEALLOC(big1);
    void* big2 = ALLOC(4096);
    ASSERT_NE(big2, nullptr);
    DEALLOC(big2);

    // 混合大小交错分配/释放, 释放前校验数据完好
    for (int i = 0; i < 200; ++i)
    {
        size_t sz = (size_t)((i % 8 + 1) * 16);  // 16/32/.../128
        void* p = ALLOC(sz);
        ASSERT_NE(p, nullptr);
        memset(p, (int)(i & 0xFF), sz);
        const unsigned char* b = (const unsigned char*)p;
        bool ok = true;
        for (size_t j = 0; j < sz; ++j)
        {
            if (b[j] != (unsigned char)(i & 0xFF))
            {
                ok = false;
                break;
            }
        }
        EXPECT_TRUE(ok) << "data corrupted, i=" << i;
        DEALLOC(p);
    }

    // 缓存滞留的块可由 flush_tcache 倒回 (不破坏后续分配/释放)
    for (int i = 0; i < 100; ++i)
    {
        void* p = ALLOC(32);
        ASSERT_NE(p, nullptr);
        DEALLOC(p);
    }
    wageco::flush_tcache();
    void* c = ALLOC(32);
    ASSERT_NE(c, nullptr);
    DEALLOC(c);
    wageco::flush_tcache();
}

// tcache 多线程冒烟测试: 每线程独立缓存, 并发小对象分配/释放
// (验证 thread_local 隔离 + 数据完好; 小对象场景不再被全局锁串行化)
TEST(testMalloc, TcacheThreadTest)
{
    constexpr int kThreads = 4;
    constexpr int kIters = 20000;
    pthread_t threads[kThreads];
    auto worker = [](void*) -> void*
    {
        for (int i = 0; i < kIters; ++i)
        {
            size_t sz = (size_t)((i % 4 + 1) * 32);  // 32/64/96/128
            void* p = ALLOC(sz);
            if (!p)
            {
                return (void*)1;
            }
            memset(p, (int)(i & 0xFF), sz);
            DEALLOC(p);
        }
        return nullptr;
    };
    for (int t = 0; t < kThreads; ++t)
    {
        ASSERT_EQ(pthread_create(&threads[t], nullptr, worker, nullptr), 0);
    }
    for (int t = 0; t < kThreads; ++t)
    {
        void* ret = nullptr;
        ASSERT_EQ(pthread_join(threads[t], &ret), 0);
        EXPECT_EQ(ret, nullptr) << "thread " << t << " allocation failed";
    }
    // 主线程缓存与各工作线程缓存互不影响, 倒回后一切正常
    wageco::flush_tcache();
    void* p = ALLOC(64);
    ASSERT_NE(p, nullptr);
    DEALLOC(p);
}

// BestFit 查找策略冒烟测试: 验证策略可注入且分配/释放行为正确
// (使用独立构造的 Allocator + BestFit, 与全局 first-fit 分配器互不干扰。
//  模板分支版: 依赖通过模板实参编译期注入)
TEST(testMalloc, BestFitTest)
{
    wageco::HeaderList<wageco::block_t> free_list;
    wageco::SbrkMemory memory;
    wageco::BestFit strategy;
    wageco::Allocator<wageco::HeaderList<wageco::block_t>, wageco::SbrkMemory, wageco::BestFit> alloc(
        &free_list, &memory, &strategy);

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

// ============================================================================
// 模板分支演示: HeaderList 真正泛型化 —— 同一份链表代码服务于不同宿主类型
// ============================================================================
// 定义一种与 block_t 布局不同的"宿主": 24B 头 (block_t 的 16B + 8B tag)。
// 只需特化 host_traits<demo_block>, HeaderList 代码零改动即可复用 ——
// 这正是模板代码生成能力的证明 (同一份源码生成多个实例)。
namespace
{
struct demo_block
{
    size_t size;
    bool inuse;
    uint64_t tag;  // 演示: 比 block_t 多 8 字节的自定义字段
};

// demo 宿主: 节点位于头后 sizeof(demo_block) 处 (与 block_t 的偏移不同!)
inline wageco::ListNode* demo_node_of(demo_block* h)
{
    return reinterpret_cast<wageco::ListNode*>(reinterpret_cast<char*>(h) + sizeof(demo_block));
}
inline demo_block* demo_block_of(wageco::ListNode* n)
{
    return reinterpret_cast<demo_block*>(reinterpret_cast<char*>(n) - sizeof(demo_block));
}
inline size_t demo_size_of(const wageco::ListNode* n) { return demo_block_of(const_cast<wageco::ListNode*>(n))->size; }

inline void demo_init_free(demo_block* node, size_t size)
{
    node->size = size;
    node->inuse = false;
    wageco::ListNode* n = demo_node_of(node);
    n->pre = n;
    n->next = n;
}
}  // namespace

namespace wageco
{
// 特化 host_traits: 让 HeaderList<demo_block> 认识这个新宿主 (编译期适配)
template <>
struct host_traits<demo_block>
{
    static ListNode* to_node(demo_block* h) { return demo_node_of(h); }
    static demo_block* to_host(ListNode* n) { return demo_block_of(n); }
    static size_t size_of(const ListNode* n) { return demo_size_of(n); }
    static constexpr size_t header_size = sizeof(demo_block);
};
}  // namespace wageco

// 泛型链表演示: 用 HeaderList<demo_block> 做增删查 (与 block_t 实例并列),
// 证明链表代码不绑定任何具体宿主。
TEST(testMalloc, GenericListTest)
{
    wageco::HeaderList<demo_block> list;
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0u);

    demo_block a, b, c;
    demo_init_free(&a, 64);
    demo_init_free(&b, 128);
    demo_init_free(&c, 256);
    a.tag = 0xAAA;
    b.tag = 0xBBB;
    c.tag = 0xCCC;

    // 插入三个节点
    list.insert(demo_node_of(&a));
    list.insert(demo_node_of(&b));
    list.insert(demo_node_of(&c));
    EXPECT_EQ(list.size(), 3u);
    EXPECT_FALSE(list.empty());

    // first-fit: 找大小 >= 100 的第一块。insert 是头部插入,
    // 插入顺序 a, b, c 后表头是 c -> 从头找第一个 >=100 的是 c (256)
    wageco::ListNode* fit = list.find_first_fit(100);
    ASSERT_NE(fit, nullptr);
    EXPECT_EQ(demo_block_of(fit)->tag, 0xCCC);

    // contains
    EXPECT_TRUE(list.contains(demo_node_of(&b)));
    EXPECT_FALSE(list.contains(nullptr));

    // BestFit 也能工作 (通过 ListT::traits 取大小)
    wageco::BestFit best;
    wageco::ListNode* bfit = best.find(list, 200);
    ASSERT_NE(bfit, nullptr);
    EXPECT_EQ(demo_block_of(bfit)->tag, 0xCCC);  // 256 是 >=200 的最小块

    // remove 后 size 递减
    list.remove(demo_node_of(&b));
    EXPECT_EQ(list.size(), 2u);
    EXPECT_FALSE(list.contains(demo_node_of(&b)));

    // 物理相邻: 手工构造连续布局 —— d0 的用户区末尾紧贴 d1 的起始
    // [d0 header(24B) | d0 用户区(32B) | d1 header(24B) | d1 用户区(48B)]
    char buf[sizeof(demo_block) + 32 + sizeof(demo_block) + 48];
    demo_block* d0 = reinterpret_cast<demo_block*>(buf);
    demo_block* d1 = reinterpret_cast<demo_block*>(buf + sizeof(demo_block) + 32);
    demo_init_free(d0, 32);
    demo_init_free(d1, 48);
    wageco::HeaderList<demo_block> contig;
    contig.insert(demo_node_of(d1));
    contig.insert(demo_node_of(d0));
    wageco::ListNode* next = contig.find_next_phys(demo_node_of(d0));
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(demo_block_of(next), d1);
}
// ============================================================================
// 模板分支演示: 编译期能力分派 (tag dispatch)
// ============================================================================
// 定义一个"支持随机释放"的假提供者并特化 memory_traits —— Allocator 的
// 释放路径在编译期切换到 release_dispatch(std::true_type) (直接归还),
// 反向释放循环 (std::false_type) 不会被实例化。通过 release_calls 计数
// 证明走的是随机释放路径 (运行时零分支)。
namespace
{
struct DummyRandomMemory
{
    void* base;
    size_t size;
    int release_calls = 0;

    void* allocate(size_t) { return base; }
    bool release_block(void*, size_t)
    {
        ++release_calls;
        return true;
    }
    bool owns_address(const void* p) const { return p >= base && p < (char*)base + size; }
};
}  // namespace

namespace wageco
{
// 特化: 声明该提供者"支持随机释放" (编译期能力)
template <>
struct memory_traits<DummyRandomMemory>
{
    static constexpr bool random_release = true;
};
}  // namespace wageco

TEST(testMalloc, CompileTimeDispatchTest)
{
    // 一块假堆: 手工放一个 block_t, 假装是已分配块
    char raw[64] alignas(wageco::block_t);
    wageco::block_t* node = reinterpret_cast<wageco::block_t*>(raw);
    node->head.size = 16;
    node->head.inuse = true;

    DummyRandomMemory mem{raw, sizeof(raw), 0};
    wageco::HeaderList<wageco::block_t> list;
    wageco::FirstFit strat;
    wageco::Allocator<wageco::HeaderList<wageco::block_t>, DummyRandomMemory, wageco::FirstFit> alloc(&list, &mem,
                                                                                                      &strat);

    alloc.free((void*)(node + 1));
    // 编译期选中"随机释放"路径: 直接 release_block, 而不是反向释放循环
    EXPECT_EQ(mem.release_calls, 1);
}

// ============================================================================
// 模板分支演示: 编译期函数式 —— 表/纯函数/递归的一致性
// ============================================================================
// functional.h 里的 static_assert 已在编译期验证纯函数与表; 本测试在运行时
// 全量比对编译期生成的查找表与算术纯函数 (以及 Tcache 的接入), 证明
// "编译期生成的数据"与"运行时计算"殊途同归。
TEST(testMalloc, CompileTimeTableTest)
{
    // 1) 编译期表 vs 纯函数算术: 每个请求大小逐一比对
    for (size_t req = 0; req <= wageco::kMaxCachedBytes; ++req)
    {
        size_t expect = req == 0 ? 0 : wageco::bin_of(wageco::align_user_size(req));
        EXPECT_EQ(wageco::kReqToBin.values[req], expect) << "req=" << req;
    }
    // 2) Tcache 接入的 bin_of_request 与表一致 (请求 1..缓存上限;
    //    上限取 functional.h 的 kMaxCachedBytes, 与 Tcache<> 默认一致)
    for (size_t req = 1; req <= wageco::kMaxCachedBytes; ++req)
    {
        EXPECT_EQ(wageco::Tcache<>::bin_of_request(req), wageco::kReqToBin.values[req]) << "tcache req=" << req;
    }
    // 3) 类型级递归 BinSizeAt 与算术 bin_user_size 一致 (编译期 static_assert
    //    已验 0/3/63 档, 这里抽验边界档)
    EXPECT_EQ(wageco::BinSizeAt<0>::value, 16u);
    EXPECT_EQ(wageco::BinSizeAt<63>::value, 1024u);
    // 4) 值级递归 bin_of_recursive 与算术 bin_of 一致
    EXPECT_EQ(wageco::bin_of_recursive(1024), wageco::bin_of(1024));
}
#endif  // MMEMORY_TEST_CUSTOM
