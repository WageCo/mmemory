// ============================================================================
// list.h - 链表层: ListNode / IList / HeaderList
// ----------------------------------------------------------------------------
// ListNode    - 链表节点 (只含 pre/next 链接指针, 独立定义)
// IList       - 链表抽象接口 (分配器只依赖该接口, 不绑定具体实现)
// HeaderList  - IList 的具体实现 (双向循环链表, 通过 SizeFn 回调取块大小)
// 块头不内嵌链表节点: 空闲块的"用户区前 16 字节"复用为 ListNode
// (见 block.h 的 node_of/init_free), 已分配块的用户区完全交给数据。
// ============================================================================
#ifndef MMEMORY_LIST_H
#define MMEMORY_LIST_H

#include <stddef.h>  // size_t

#include <functional>  // std::function

namespace wageco
{
// ----------------------------------------------------------------------------
// ListNode - 链表节点 (独立定义, 与块头完全解耦)
// ----------------------------------------------------------------------------
// 只负责链接 (前驱/后继)。节点不占用块头空间: 它"复用"空闲块的用户区
// 前 16 字节 (空闲块没有用户数据), 见 block.h 的 node_of()/init_free()。
struct ListNode
{
    ListNode* pre;   // 前驱
    ListNode* next;  // 后继
};

// ----------------------------------------------------------------------------
// IList - 链表抽象接口 (依赖注入点)
// ----------------------------------------------------------------------------
// 分配器只依赖本接口, 不关心具体实现 (双向链表 / 分 bin 表 / 树等)。
// 任何新实现只需继承本接口并实现全部纯虚函数。
// 性能注记: 虚函数调用 (~ns 级) 相对分配器主要成本 (锁 + sbrk, ~µs 级)
//   可忽略, benchmark 已佐证。
class IList
{
   public:
    virtual ~IList() = default;

    // 是否为空
    virtual bool empty() const = 0;

    // 当前节点数
    virtual size_t size() const = 0;

    // 把 node 插入链表头部 (node 需先经 init_free 初始化)
    virtual void insert(ListNode* node) = 0;

    // first-fit: 返回第一个"宿主块大小 >= size"的节点 (只查找, 不摘除)
    virtual ListNode* find_first_fit(size_t size) const = 0;

    // 判断 node 是否在链表中 (备用; free 主要用 inuse 标志校验)
    virtual bool contains(ListNode* node) const = 0;

    // 物理地址紧邻 node 之前的节点 (释放时向前合并用)
    virtual ListNode* find_prev_phys(ListNode* node) const = 0;

    // 物理地址紧邻 node 之后的节点 (释放时向后合并用)
    virtual ListNode* find_next_phys(ListNode* node) const = 0;

    // 摘除 node (调用方需保证 node 在链表中), 并把 node 恢复为孤立节点
    virtual void remove(ListNode* node) = 0;

    // 遍历链表中所有节点 (供需要全表扫描的策略/统计使用)
    virtual void for_each(const std::function<void(ListNode*)>& fn) const = 0;
};

// ----------------------------------------------------------------------------
// HeaderList - IList 的具体实现: 双向循环链表
// ----------------------------------------------------------------------------
// 职责:
//   - 维护循环链表 (头指针) 与节点计数 count, insert/remove 自动维护计数;
//   - 块大小的获取通过 SizeFn 回调 (由分配器提供), 链表不关心数据如何存放;
//   - 扩展点: 以后可按大小分 bin (一个分配器持有多个 HeaderList)、
//     增加字节总量统计等, 都只需在此类上做增量修改。
// 实现位置: 定义在 src/list.cpp (声明/实现分离); 简单访问器保留内联。
class HeaderList : public IList
{
   public:
    // 从链表节点获取其宿主块大小的回调 (解耦的关键: 链表不直接认识宿主类型)
    using SizeFn = size_t (*)(const ListNode* node);

    explicit HeaderList(SizeFn size_fn) : head_(nullptr), count_(0), size_fn_(size_fn) {}

    // --- IList 接口实现 (定义见 src/list.cpp) ---
    bool empty() const override { return head_ == nullptr; }
    size_t size() const override { return count_; }
    void insert(ListNode* node) override;
    ListNode* find_first_fit(size_t size) const override;
    bool contains(ListNode* node) const override;
    ListNode* find_prev_phys(ListNode* node) const override;
    ListNode* find_next_phys(ListNode* node) const override;
    void remove(ListNode* node) override;
    void for_each(const std::function<void(ListNode*)>& fn) const override;

   private:
    ListNode* head_;  // 循环链表头 (nullptr 表示空表)
    size_t count_;    // 节点数
    SizeFn size_fn_;  // 获取节点对应宿主块大小的回调
};

}  // namespace wageco
#endif  // MMEMORY_LIST_H
