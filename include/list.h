// ============================================================================
// list.h - 链表层: ListNode / host_traits / HeaderList (模板版, 真正泛型)
// ----------------------------------------------------------------------------
// 本分支 (template_c++11) 的模板化要点 (对比 master 的虚函数注入版):
//   - IList 虚接口被删除, 依赖通过模板参数编译期注入 (header-only);
//   - HeaderList 真正泛型化: 模板参数从"块大小回调 SizeFn"提升为
//     "宿主类型 HostT" —— 链表代码不再认识 block_t, 它只认识 ListNode,
//     宿主 <-> 节点互转 / 取大小 / 头大小全部由 host_traits<HostT>
//     适配器在编译期提供。换宿主 (如不同块头布局) 只需特化 host_traits,
//     链表代码零改动 (同一份代码生成多个实例);
//   - for_each 模板化 (任意可调用对象), 替代 master 的 std::function。
// ListNode 保留: 只含 pre/next 链接指针, 独立定义, 与块头解耦。
// 块头不内嵌链表节点: 空闲块的"用户区前 16 字节"复用为 ListNode
// (见 block.h 的 node_of 与本头 init_free), 已分配块的用户区完全交给数据。
// ============================================================================
#ifndef MMEMORY_LIST_H
#define MMEMORY_LIST_H

#include <stddef.h>  // size_t
#include <stdint.h>  // uintptr_t (地址比较, 避免指针比较 UB)

#include "block.h"  // block_t / node_of / block_of / block_size_of

namespace wageco
{
// ----------------------------------------------------------------------------
// ListNode - 链表节点 (独立定义, 与块头完全解耦)
// ----------------------------------------------------------------------------
// 只负责链接 (前驱/后继)。节点不占用块头空间: 它"复用"空闲块的用户区
// 前 16 字节 (空闲块没有用户数据), 见 block.h 的 node_of()/本头 init_free()。
struct ListNode
{
    ListNode* pre;   // 前驱
    ListNode* next;  // 后继
};

// 初始化"空闲"块 (block_t 宿主): 用户区前段复用为链表节点 (节点自环)
// (需要 ListNode 完整定义, 故放在本头; master 版在 block.h)
inline void init_free(block_t* node, size_t size)
{
    node->head.size = size;
    node->head.inuse = false;
    ListNode* n = node_of(node);
    n->pre = n;
    n->next = n;
}

// ----------------------------------------------------------------------------
// host_traits - 宿主类型适配器 (编译期"类型接口")
// ----------------------------------------------------------------------------
// 链表只认识 ListNode; 如何把 ListNode 解释为"宿主块"(取宿主指针、取大小、
// 取头大小) 由 host_traits 提供。这是模板版"泛型化"的核心:
//   - 对任意宿主类型 HostT 特化 host_traits, 即可复用同一份 HeaderList 代码;
//   - 默认不提供定义 (强制特化), 编译期保证"宿主必须可适配", 避免忘特化;
//   - 块头大小 header_size 是编译期常量 —— 物理相邻计算 (地址算术) 依赖它。
template <typename HostT>
struct host_traits;

// block_t 宿主: 节点位于头后 sizeof(block_t) 处, 大小即 head.size
template <>
struct host_traits<block_t>
{
    static ListNode* to_node(block_t* h) { return node_of(h); }
    static block_t* to_host(ListNode* n) { return block_of(n); }
    static size_t size_of(const ListNode* n) { return block_size_of(n); }
    static constexpr size_t header_size = sizeof(block_t);
};

// ----------------------------------------------------------------------------
// HeaderList - 泛型双向循环链表 (编译期绑定宿主类型 HostT)
// ----------------------------------------------------------------------------
// 循环不变量 (与 master 一致):
//   - 链表是【循环】的: 头节点的 pre 指向尾节点, 尾节点的 next 指向头节点,
//     因此不需要单独的尾指针 —— "头的前驱即尾";
//   - 单节点时, 该节点的 pre == next == 自己 (最小环);
//   - 空表 head_ == nullptr。
// 模板参数:
//   HostT - 宿主类型 (如 block_t); 节点 <-> 宿主互转/取大小由
//           host_traits<HostT> 编译期提供。
// 职责:
//   - 维护循环链表 (头指针) 与节点计数 count, insert/remove 自动维护计数;
//   - 链表不关心宿主如何存放数据, 一切通过 host_traits<HostT> 访问;
//   - 扩展点: 以后可按大小分 bin (一个分配器持有多个 HeaderList)、
//     增加字节总量统计等, 都只需在此类上做增量修改。
template <typename HostT>
class HeaderList
{
   public:
    using traits = host_traits<HostT>;  // 暴露适配器 (供 BestFit 等外部使用)

    HeaderList() = default;

    // 是否为空
    bool empty() const { return head_ == nullptr; }

    // 当前节点数
    size_t size() const { return count_; }

    // 把 node 插入链表头部 (node 需先经宿主对应的 init_free 初始化)。
    // 循环链表不需要尾指针: 头的前驱即尾。
    void insert(ListNode* node)
    {
        if (head_)
        {
            // 非空表: 在头节点之前插入 (即链表尾部), 并更新头
            head_->pre->next = node;  // 尾->next = node
            node->pre = head_->pre;   // node->pre = 原尾
            node->next = head_;       // node->next = 原头
            head_->pre = node;        // 原头->pre = node
        }
        head_ = node;  // 空表时 node 自成环, 成为头
        ++count_;
    }

    // first-fit: 返回第一个"宿主块大小 >= size"的节点 (只查找, 不摘除)。
    // (历史版本是精确匹配 == size, 空闲块几乎无法复用, 造成碎片泄漏;
    //  改为 first-fit 后配合 malloc 里的 split, 空闲块才能被充分复用)
    ListNode* find_first_fit(size_t size) const
    {
        if (!head_)
        {
            return nullptr;
        }
        ListNode* node = head_;
        // 从头遍历, 跳过所有 size 不足的块; 走到头即停 (循环链表判空)
        for (; traits::size_of(node) < size && node->next != head_; node = node->next);
        return traits::size_of(node) >= size ? node : nullptr;
    }

    // 判断 node 是否在链表中 (用于 free 的合法性校验)
    bool contains(ListNode* node) const
    {
        if (!head_)
        {
            return false;
        }
        ListNode* p = head_;
        for (; p != node && p->next != head_; p = p->next);
        return p == node;
    }

    // 物理地址紧邻 node 之前的节点。
    // 物理相邻判断: 块 p 的末尾 (宿主起始 + 头大小 + 用户区大小) 正好是
    // node 宿主块的起始地址。用于释放时的向前合并 (coalescing)。
    // 宿主相关计算全部走 host_traits (编译期绑定)。
    ListNode* find_prev_phys(ListNode* node) const
    {
        if (!head_)
        {
            return nullptr;
        }
        const uintptr_t target = reinterpret_cast<uintptr_t>(traits::to_host(node));
        ListNode* p = head_;
        do
        {
            const uintptr_t p_end =
                reinterpret_cast<uintptr_t>(traits::to_host(p)) + traits::header_size + traits::size_of(p);
            if (p_end == target)
            {
                return p;
            }
            p = p->next;
        } while (p != head_);
        return nullptr;
    }

    // 物理地址紧邻 node 之后的节点 (判断: 某块起始 == node 宿主块的末尾地址)。
    // 用于释放时的向后合并。
    ListNode* find_next_phys(ListNode* node) const
    {
        if (!head_)
        {
            return nullptr;
        }
        const uintptr_t node_end =
            reinterpret_cast<uintptr_t>(traits::to_host(node)) + traits::header_size + traits::size_of(node);
        ListNode* q = head_;
        do
        {
            if (reinterpret_cast<uintptr_t>(traits::to_host(q)) == node_end)
            {
                return q;
            }
            q = q->next;
        } while (q != head_);
        return nullptr;
    }

    // 遍历链表中所有节点 (供需要全表扫描的策略/统计使用)。
    // 模板版: 接收任意可调用对象 (lambda/函数指针), 替代 master 的 std::function。
    template <typename Fn>
    void for_each(Fn fn) const
    {
        if (!head_)
        {
            return;
        }
        ListNode* p = head_;
        do
        {
            fn(p);
            p = p->next;
        } while (p != head_);
    }

    // 摘除 node (调用方需保证 node 在链表中), 并把 node 恢复为孤立节点。
    // 计数自动 -1。
    void remove(ListNode* node)
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
                head_ = head_->next;  // 摘的是头, 则后移头指针
            }
            node->pre->next = node->next;  // 前驱跳过 node
            node->next->pre = node->pre;   // 后继跳过 node
        }
        node->pre = node;  // node 变回孤立节点
        node->next = node;
        --count_;
    }

   private:
    ListNode* head_ = nullptr;  // 循环链表头 (nullptr 表示空表)
    size_t count_ = 0;          // 节点数
};

}  // namespace wageco
#endif  // MMEMORY_LIST_H
