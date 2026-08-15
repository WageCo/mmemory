// ============================================================================
// find_strategy.h - 空闲块查找策略 (分配策略, 策略模式)
// ----------------------------------------------------------------------------
// 从空闲链表中找一个满足 size 的块 (只查找, 不摘除), 不同的策略在
// 碎片率与查找开销之间取舍:
//   - FirstFit: 第一个"宿主块大小 >= size"的块 (快, 碎片略多);
//   - BestFit : 大小 >= size 且最小的块 (碎片最小, 但需要全表扫描)。
// Allocator 通过注入的策略查找空闲块 (默认 FirstFit), 换策略零改动。
// ============================================================================
#ifndef MMEMORY_FIND_STRATEGY_H
#define MMEMORY_FIND_STRATEGY_H

#include <stddef.h>  // size_t

#include "block.h"  // block_size_of (BestFit 需要比较块大小)
#include "list.h"   // IList / ListNode

namespace wageco
{
// 空闲块查找策略接口
class IFindStrategy
{
   public:
    virtual ~IFindStrategy() = default;

    // 从空闲链表中找一个"宿主块大小 >= size"的节点 (只查找, 不摘除);
    // 找不到返回 nullptr
    virtual ListNode* find(IList* free_list, size_t size) = 0;
};

// first-fit: 返回第一个满足的块 (只找第一个, 无需全表扫描)
class FirstFit : public IFindStrategy
{
   public:
    ListNode* find(IList* free_list, size_t size) override { return free_list->find_first_fit(size); }
};

// best-fit: 返回大小 >= size 且最小的块 (碎片最小, 需要遍历全表)
class BestFit : public IFindStrategy
{
   public:
    ListNode* find(IList* free_list, size_t size) override
    {
        ListNode* best = nullptr;
        size_t best_size = 0;
        free_list->for_each(
            [&](ListNode* node)
            {
                size_t node_size = block_size_of(node);
                if (node_size >= size && (!best || node_size < best_size))
                {
                    best = node;
                    best_size = node_size;
                }
            });
        return best;
    }
};

}  // namespace wageco
#endif  // MMEMORY_FIND_STRATEGY_H
