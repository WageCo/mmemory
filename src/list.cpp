// ============================================================================
// list.cpp - HeaderList 双向循环链表实现
// ----------------------------------------------------------------------------
// 类声明在 include/internal.h (声明/实现分离, 标准 C++ 布局)。
// 链表只操作 Node (链接指针), 块大小通过 size_fn_ 回调从宿主块获取,
// 物理地址通过 header_of()/node_of() 在节点与宿主块之间转换 —— 与
// 分配器业务数据解耦。
// 性能注记: 非内联引入的函数调用开销 (~ns 级) 相对分配器的主要成本
//   (全局锁 + sbrk 系统调用, 单次 ~µs 级) 可忽略, benchmark 已佐证。
// ============================================================================
#include "internal.h"

namespace wageco
{
// 把 node 插入链表头部 (node 需先经 list_init 初始化)。
// 循环链表不需要尾指针: 头的前驱即尾。
void HeaderList::insert(Node *node)
{
    if (head_)
    {
        // 非空表: 在头节点之前插入 (即链表尾部), 并更新头
        head_->pre->next = node; // 尾->next = node
        node->pre = head_->pre;  // node->pre = 原尾
        node->next = head_;      // node->next = 原头
        head_->pre = node;       // 原头->pre = node
    }
    head_ = node; // 空表时 node 自成环, 成为头
    ++count_;
}

// first-fit: 返回第一个"宿主块大小 >= size"的节点 (只查找, 不摘除)。
// (历史版本是精确匹配 == size, 空闲块几乎无法复用, 造成碎片泄漏;
//  改为 first-fit 后配合 malloc 里的 split, 空闲块才能被充分复用)
HeaderList::Node *HeaderList::find_first_fit(size_t size) const
{
    if (!head_)
    {
        return nullptr;
    }
    Node *node = head_;
    // 从头遍历, 跳过所有 size 不足的块; 走到头即停 (循环链表判空)
    for (; size_fn_(node) < size && node->next != head_; node = node->next)
        ;
    return size_fn_(node) >= size ? node : nullptr;
}

// 判断 node 是否在链表中 (用于 free 的合法性校验)
bool HeaderList::contains(Node *node) const
{
    if (!head_)
    {
        return false;
    }
    Node *p = head_;
    for (; p != node && p->next != head_; p = p->next)
        ;
    return p == node;
}

// 物理地址紧邻 node 之前的节点。
// 物理相邻判断: 块 p 的末尾 (宿主起始 + header 大小 + 用户区大小) 正好是
// node 宿主块的起始地址。用于释放时的向前合并 (coalescing)。
HeaderList::Node *HeaderList::find_prev_phys(Node *node) const
{
    if (!head_)
    {
        return nullptr;
    }
    Node *p = head_;
    do
    {
        if ((char *)header_of(p) + sizeof(header_t) + size_fn_(p) == (char *)header_of(node))
        {
            return p;
        }
        p = p->next;
    } while (p != head_);
    return nullptr;
}

// 物理地址紧邻 node 之后的节点 (判断: 某块起始 == node 宿主块的末尾地址)。
// 用于释放时的向后合并。
HeaderList::Node *HeaderList::find_next_phys(Node *node) const
{
    if (!head_)
    {
        return nullptr;
    }
    char *node_end = (char *)header_of(node) + sizeof(header_t) + size_fn_(node);
    Node *q = head_;
    do
    {
        if ((char *)header_of(q) == node_end)
        {
            return q;
        }
        q = q->next;
    } while (q != head_);
    return nullptr;
}

// 摘除 node (调用方需保证 node 在链表中), 并把 node 恢复为孤立节点。
// 计数自动 -1。
void HeaderList::remove(Node *node)
{
    if (!head_)
    {
        return;
    }
    if (head_->next == head_)
    {
        // 链表只剩这一个节点: 摘除后整表为空
        head_ = nullptr;
    }
    else
    {
        if (node == head_)
        {
            head_ = head_->next; // 摘的是头, 则后移头指针
        }
        node->pre->next = node->next; // 前驱跳过 node
        node->next->pre = node->pre;  // 后继跳过 node
    }
    node->pre = node; // node 变回孤立节点
    node->next = node;
    --count_;
}
} // namespace wageco
