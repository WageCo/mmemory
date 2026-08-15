// ============================================================================
// mmemory.cpp - 教学用简易内存分配器: 对外 API (转发到全局分配器)
// ----------------------------------------------------------------------------
// 模块结构 (include/ + src/):
//   internal.h    - 内部共享: ListNode / IList / HeaderList / block_t /
//                   Allocator 声明 + 日志配置
//   list.cpp      - HeaderList: IList 的具体实现 (双向循环链表)
//   allocator.cpp - Allocator 核心分配逻辑 (通过注入的 IList* 操作列表)
//   log.cpp       - 日志系统实现 (spdlog 封装, 非热路径)
//   mmemory.cpp   - 装配 (创建链表实现并注入分配器) + 四个对外 API (转发)
//
// 依赖注入: 本文件是"组合根"——创建空闲链表实现, 通过 Allocator 构造函数
//   注入 (策略模式)。以后想换链表实现 (如按大小分 bin),
//   只需在这里换成新的 IList 实现, 其余代码零改动。
//
// 核心思路 (与经典 malloc 实现一脉相承):
//   1. 用 sbrk() 系统调用从操作系统申请/归还堆空间;
//   2. 每块内存前都有一个 header (元数据), 记录块大小与链表节点;
//   3. Allocator 通过注入的空闲链表 (free_list) 管理空闲块;
//      已分配状态由块头的 inuse 标志标识 (边界 tag 思路)。
//
// 支持的 API (位于 namespace wageco, 与标准库同名):
//   wageco::malloc / wageco::free / wageco::calloc / wageco::realloc
//
// 注意: 这是教学实现, 追求"把原理讲清楚", 不做性能优化。
//   与系统 malloc 相比, 它没有 tcache / per-thread arena 等加速机制,
//   每次分配都可能有 sbrk 系统调用, 释放时链表查找是 O(n) 的,
//   并发全靠一把全局互斥锁串行化 —— 详见 README 中的 benchmark 对比。
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
// 组合根 (dependency composition root): 装配"存储模式 + 内存申请"并注入分配器
// ----------------------------------------------------------------------------
// 空闲块链表实现 (存储模式: 双循环链表, 块大小回调读取; 已分配状态由
// 块的 inuse 标志标识, 因此不再需要"已分配链表")
static HeaderList g_free_list(block_size_of);

// 空闲块查找策略 (默认 first-fit; 换 best-fit 只需改这里)
static FirstFit g_first_fit;

// 内存申请来源 (基于 sbrk 的内存提供者)
static SbrkMemory g_memory;

// 全局分配器实例: 注入"存储模式 + 内存申请 + 查找策略"三个依赖
static Allocator g_allocator(&g_free_list, &g_memory, &g_first_fit);

// ----------------------------------------------------------------------------
// 对外 API: 转发到全局分配器实例
// ----------------------------------------------------------------------------
void* malloc(size_t size) { return g_allocator.malloc(size); }

void free(void* addr) { g_allocator.free(addr); }

void* calloc(size_t num, size_t size) { return g_allocator.calloc(num, size); }

void* realloc(void* addr, size_t size) { return g_allocator.realloc(addr, size); }
}  // namespace wageco
