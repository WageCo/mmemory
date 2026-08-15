// ============================================================================
// list.cpp - HeaderList 双向循环链表实现
// ----------------------------------------------------------------------------
// 类声明在 include/internal.h (声明/实现分离, 标准 C++ 布局)。
// 性能注记: 非内联引入的函数调用开销 (~ns 级) 相对分配器的主要成本
//   (全局锁 + sbrk 系统调用, 单次 ~µs 级) 可忽略, benchmark 已佐证。
// ============================================================================
#include "internal.h"

namespace wageco
{
// 把 node 插入链表头部 (node 需先经 list_init 初始化)。
// 循环链表不需要尾指针: 头的前驱即尾。
void HeaderList::insert(header_t *node)
{
    if (head_)
    {
        // 非空表: 在头节点之前插入 (即链表尾部), 并更新头
        head_->head.pre->head.next = node; // 尾->next = node
        node->head.pre = head_->head.pre;  // node->pre = 原尾
        node->head.next = head_;           // node->next = 原头
        head_->head.pre = node;            // 原头->pre = node
    }
    head_ = node; // 空表时 node 自成环, 成为头
    ++count_;
}

// first-fit: 返回第一个"用户可用区 >= size"的块 (只查找, 不摘除)。
// (历史版本是精确匹配 == size, 空闲块几乎无法复用, 造成碎片泄漏;
//  改为 first-fit 后配合 malloc 里的 split, 空闲块才能被充分复用)
header_t *HeaderList::find_first_fit(size_t size) const
{
    if (!head_)
    {
        return nullptr;
    }
    header_t *node = head_;
    // 从头遍历, 跳过所有 size 不足的块; 走到头即停 (循环链表判空)
    for (; node->head.size < size && node->head.next != head_; node = node->head.next)
        ;
    return node->head.size >= size ? node : nullptr;
}

// 判断 node 是否在链表中 (用于 free 的合法性校验)
bool HeaderList::contains(header_t *node) const
{
    if (!head_)
    {
        return false;
    }
    header_t *p = head_;
    for (; p != node && p->head.next != head_; p = p->head.next)
        ;
    return p == node;
}

// 物理地址紧邻 node 之前的块。
// 物理相邻判断: 块 p 的末尾 (header + 用户区末尾) 正好是 node 的起始地址。
// 用于释放时的向前合并 (coalescing)。
header_t *HeaderList::find_prev_phys(header_t *node) const
{
    if (!head_)
    {
        return nullptr;
    }
    header_t *p = head_;
    do
    {
        if ((char *)p + sizeof(header_t) + p->head.size == (char *)node)
        {
            return p;
        }
        p = p->head.next;
    } while (p != head_);
    return nullptr;
}

// 物理地址紧邻 node 之后的块 (判断: 某块起始 == node 的末尾地址)。
// 用于释放时的向后合并。
header_t *HeaderList::find_next_phys(header_t *node) const
{
    if (!head_)
    {
        return nullptr;
    }
    char *node_end = (char *)node + sizeof(header_t) + node->head.size;
    header_t *q = head_;
    do
    {
        if ((char *)q == node_end)
        {
            return q;
        }
        q = q->head.next;
    } while (q != head_);
    return nullptr;
}

// 摘除 node (调用方需保证 node 在链表中), 并把 node 恢复为孤立节点。
// 计数自动 -1。
void HeaderList::remove(header_t *node)
{
    if (!head_)
    {
        return;
    }
    if (head_->head.next == head_)
    {
        // 链表只剩这一个节点: 摘除后整表为空
        head_ = nullptr;
    }
    else
    {
        if (node == head_)
        {
            head_ = head_->head.next; // 摘的是头, 则后移头指针
        }
        node->head.pre->head.next = node->head.next; // 前驱跳过 node
        node->head.next->head.pre = node->head.pre;  // 后继跳过 node
    }
    list_init(node, node->head.size); // node 变回孤立节点
    --count_;
}
} // namespace wageco
