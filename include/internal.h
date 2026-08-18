// ============================================================================
// internal.h - mmemory 内部总头 (聚合各分头, 供库内 src/*.cpp 使用)
// ----------------------------------------------------------------------------
// 按职责分层 (头文件统一放 include/, 均为内部实现细节, 非公共接口)。
// 本分支 (template_c++11) 为编译期多态版: 虚接口 (IList/IMemory/
// IFindStrategy) 全部删除, 依赖通过模板参数注入, 头文件依赖方向:
//   block.h         块层: align_to / block_t / node_of / block_of /
//                   block_size_of (ListNode 仅前向声明, 无依赖)
//   list.h          链表层: ListNode / init_free / host_traits / HeaderList<HostT>
//                   (包含 block.h, 模板化, header-only)
//   functional.h    编译期函数式层: constexpr 纯函数 / 模板递归 / 表生成
//   memory.h        内存提供者层: SbrkMemory + memory_traits (编译期能力)
//   find_strategy.h 查找策略层: FirstFit / BestFit (模板成员函数)
//   tcache.h        线程本地缓存层: Tcache<kMaxBytes, kBinLimit>
//   allocator.h     分配器层: Allocator<ListT, MemoryT, StrategyT>
//   logging.h       日志配置 (SPDLOG_ACTIVE_LEVEL) + get_logger 声明
//
// 物理解耦 (边界 tag 思路, 与 dlmalloc 一致, 与 master 相同):
//   - block_t 不内嵌链表节点, 只有 size 和 inuse 标志;
//   - 空闲块的"用户区前 16 字节"复用为 ListNode (空闲块没有用户数据,
//     节点零额外开销); 已分配块的用户区完全交给数据;
//   - 因此"已分配链表"不再需要: free 通过 inuse 标志校验,
//     物理相邻的空闲块通过链表查找定位。
// ============================================================================
#ifndef MMEMORY_INTERNAL_H
#define MMEMORY_INTERNAL_H

#include "allocator.h"
#include "block.h"
#include "find_strategy.h"
#include "functional.h"
#include "list.h"
#include "logging.h"
#include "memory.h"
#include "tcache.h"

#endif  // MMEMORY_INTERNAL_H
