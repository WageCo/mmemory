// ============================================================================
// find_strategy.h - 空闲块查找策略 (模板分支版, 编译期多态)
// ----------------------------------------------------------------------------
// 本分支 (template_c++11) 的模板化要点 (对比 master):
//   - IFindStrategy 虚接口被删除: 策略变成普通 struct, find 是模板成员函数,
//     接收任意"符合链表接口形状"的类型 (编译期鸭子类型) ——
//     编译期绑定, 零虚函数开销;
//   - FirstFit: 第一个"宿主块大小 >= size"的块 (快, 碎片略多);
//   - BestFit : 大小 >= size 且最小的块 (碎片最小, 需要全表扫描)。
// Allocator 通过模板参数注入策略 (默认 FirstFit), 换策略零改动。
// ============================================================================
#ifndef MMEMORY_FIND_STRATEGY_H
#define MMEMORY_FIND_STRATEGY_H

#include <stddef.h>  // size_t

#include "block.h"  // block_size_of / ListNode (BestFit 需要比较块大小)

namespace wageco
{
// ----------------------------------------------------------------------------
// FirstFit - 第一个满足的块 (只找第一个, 无需全表扫描)
// ----------------------------------------------------------------------------
struct FirstFit
{
    // 模板成员: 编译期绑定任意链表类型 (只需有 find_first_fit(size))
    template <typename ListT>
    ListNode* find(ListT& free_list, size_t size) const
    {
        return free_list.find_first_fit(size);
    }
};

// ----------------------------------------------------------------------------
// BestFit - 大小 >= size 且最小的块 (碎片最小, 需要遍历全表)
// ----------------------------------------------------------------------------
struct BestFit
{
    // 模板成员: 编译期绑定任意链表类型 (只需有 for_each(可调用))
    template <typename ListT>
    ListNode* find(ListT& free_list, size_t size) const
    {
        ListNode* best = nullptr;
        size_t best_size = 0;
        free_list.for_each(
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
