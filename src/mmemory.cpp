// ============================================================================
// mmemory.cpp - 教学用简易内存分配器: 对外 API (转发到全局分配器)
// ----------------------------------------------------------------------------
// 本分支 (template_c++11) 为编译期多态版 (对比 master 的虚函数注入版):
//   - 依赖 (链表/内存/策略/tcache) 全部通过模板参数注入, 零虚函数;
//   - 组合根装配的模板实参:
//       HeaderList<block_size_of>  ← 存储模式 (SizeFn 编译期绑定)
//       SbrkMemory                 ← 内存申请 (普通类, 非虚方法)
//       FirstFit                   ← 查找策略 (模板成员 find)
//       Tcache<1024, 7>            ← 线程本地缓存 (上限/块数模板参数)
//   - 公共 API (wageco::malloc/free/calloc/realloc/flush_tcache) 与 master
//     完全一致。
//
// 模块结构 (include/ + src/):
//   internal.h    - 内部共享: ListNode / HeaderList / block_t / Allocator /
//                   Tcache 声明 (模板版全部在头文件) + 日志配置
//   mmemory.cpp   - 装配 (模板实例化 + thread_local tcache) + 对外 API
//   log.cpp       - 日志系统实现 (spdlog 封装, 非热路径)
//
// 核心思路 (与 master 一脉相承, 与经典 malloc 实现同源):
//   1. 用 sbrk() 系统调用从操作系统申请/归还堆空间;
//   2. 每块内存前都有一个 header (元数据), 记录块大小与链表节点;
//   3. Allocator 通过注入的空闲链表 (free_list) 管理空闲块;
//      已分配状态由块头的 inuse 标志标识 (边界 tag 思路);
//   4. 公共 API 前再加一层 per-thread tcache: 小对象 (<= 1024B) 的
//      分配/释放先命中本线程缓存 —— 零锁、零系统调用、零链表查找。
//
// 支持的 API (位于 namespace wageco, 与标准库同名):
//   wageco::malloc / wageco::free / wageco::calloc / wageco::realloc
//   wageco::flush_tcache()  把当前线程缓存中的块全部倒回全局分配器
//
// 日志: 使用 spdlog (见 internal.h / log.cpp)。
//   默认 stderr 输出; 级别: DEBUG 编译默认 debug, 否则 info; 环境变量可覆盖:
//     MMEMORY_LOG_LEVEL=trace|debug|info|warn|error|critical|off
//     MMEMORY_LOG_FILE=<path>  指定则改为写文件 (追加), 否则 stderr
// ============================================================================

#include "internal.h"

namespace wageco
{
// ----------------------------------------------------------------------------
// 组合根 (dependency composition root): 模板实参注入"存储+内存+策略"
// ----------------------------------------------------------------------------
// 空闲块链表实现 (存储模式: 双循环链表, 宿主 = block_t —— 节点↔宿主互转/
// 取大小由 host_traits<block_t> 编译期提供; 已分配状态由块的 inuse 标志
// 标识, 因此不再需要"已分配链表")
static HeaderList<block_t> g_free_list;

// 空闲块查找策略 (默认 first-fit; 换 best-fit 只需改这里的模板实参)
static FirstFit g_first_fit;

// 内存申请来源 (基于 sbrk 的内存提供者, 普通类;
// 能力由 memory_traits<SbrkMemory> 编译期描述: 非随机释放)
static SbrkMemory g_memory;

// 全局分配器实例: 编译期注入"存储模式 + 内存申请 + 查找策略"三个依赖
static Allocator<HeaderList<block_t>, SbrkMemory, FirstFit> g_allocator(&g_free_list, &g_memory, &g_first_fit);

// ----------------------------------------------------------------------------
// 线程本地缓存 (tcache): 公共 API 前的 per-thread 快路径
// ----------------------------------------------------------------------------
// 小对象 (请求 <= Tcache<>::kMaxBytes) 的 malloc/free 先查本线程缓存:
// 命中则零锁、零系统调用、零链表查找; 档满时整档倒回全局分配器。
// 倒回回调 = 全局分配器的 free: 缓存块回到空闲链表, 参与物理合并与
// 反向释放 —— 缓存只延迟回收, 不阻止回收。
// 声明顺序注意: tls_cache 必须声明在 g_allocator 之后 —— 线程退出时
// thread_local 析构 (倒回全部缓存块) 先于静态对象析构, 保证倒回时
// g_allocator 仍存活 (DEBUG 构建的泄漏检测也依赖此顺序)。
static void drain_to_allocator(block_t* node) { g_allocator.free((void*)(node + 1)); }
thread_local Tcache<> tls_cache(drain_to_allocator);

// ----------------------------------------------------------------------------
// 对外 API: 先查线程本地缓存, 未命中再转发到全局分配器
// ----------------------------------------------------------------------------
void* malloc(size_t size)
{
    // 快路径: 小对象先查本线程缓存 (零锁 / 零系统调用 / 零链表查找)。
    // size==0 保持分配器的语义 (返回 nullptr), 不走缓存。
    if (size && Tcache<>::covers(size))
    {
        void* p = tls_cache.pop(Tcache<>::bin_of_request(size));
        if (p)
        {
            return p;
        }
    }
    return g_allocator.malloc(size);
}

void free(void* addr)
{
    if (!addr)
    {
        return;  // free(NULL) 是合法空操作
    }
    block_t* node = (block_t*)addr - 1;  // 回退 16 字节拿到块头
    // 快路径: 小对象压入本线程缓存 (零锁 / 零系统调用)。
    if (Tcache<>::covers(node->head.size))
    {
        // 合法性校验 (与 Allocator::free 一致): 地址必须在提供者空间内,
        // 且 inuse 必须为 true (防 double free / 非法指针)。先查地址再读
        // 块头, 避免对野指针解引用。
        if (!g_memory.owns_address(node) || !node->head.inuse)
        {
            get_logger()->error("free: double free or no malloc, start: {:p}", (void*)node);
            return;
        }
        node->head.inuse = false;  // 标记空闲 (在缓存中)
        const size_t bin = Tcache<>::bin_of_block(node->head.size);
        if (tls_cache.full(bin))
        {
            tls_cache.flush_bin(bin);  // 档满: 整档倒回全局分配器再放新块
        }
        tls_cache.push(node);
        return;
    }
    g_allocator.free(addr);
}

// 把当前线程缓存中的全部缓存块倒回全局分配器 (空闲链表)。
// 之后这些块可被合并/反向释放回系统。用于程序退出、测试或任何
// 需要"缓存不滞留内存"的场景 (如碎片回收验证)。
void flush_tcache() { tls_cache.flush_all(); }

void* calloc(size_t num, size_t size) { return g_allocator.calloc(num, size); }

// 教学取舍: calloc/realloc 直接转发到全局分配器, 不接入 tcache 快路径
// (冷路径; Allocator 内部的 malloc/free 调用不会重复进入 tcache, 语义正确,
//  只是少一层缓存加速 —— 保持分配器层逻辑不变)。
void* realloc(void* addr, size_t size) { return g_allocator.realloc(addr, size); }
}  // namespace wageco
