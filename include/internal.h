// ============================================================================
// internal.h - mmemory 内部总头 (聚合各分头, 供库内 src/*.cpp 使用)
// ----------------------------------------------------------------------------
// 按职责分层 (头文件统一放 include/, 均为内部实现细节, 非公共接口):
//   logging.h      日志配置 (SPDLOG_ACTIVE_LEVEL) + get_logger 声明
//   list.h         链表层: ListNode / IList / HeaderList
//   block.h        块层: 对齐 / block_t / 块-节点互转 helpers (依赖 list.h)
//   memory.h       内存提供者层: IMemory / SbrkMemory
//   find_strategy.h 查找策略层: IFindStrategy / FirstFit / BestFit
//   allocator.h    分配器层: Allocator (依赖以上各层)
//
// 物理解耦 (边界 tag 思路, 与 dlmalloc 一致):
//   - block_t 不内嵌链表节点, 只有 size 和 inuse 标志;
//   - 空闲块的"用户区前 16 字节"复用为 ListNode (空闲块没有用户数据,
//     节点零额外开销); 已分配块的用户区完全交给数据;
//   - 因此"已分配链表"不再需要: free 通过 inuse 标志校验,
//     物理相邻的空闲块通过地址计算 (block + size) 直接定位。
// ============================================================================
#ifndef MMEMORY_INTERNAL_H
#define MMEMORY_INTERNAL_H

#include "allocator.h"
#include "block.h"
#include "find_strategy.h"
#include "list.h"
#include "logging.h"
#include "memory.h"

#endif  // MMEMORY_INTERNAL_H
