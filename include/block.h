// ============================================================================
// block.h - 块层: 对齐 / block_t / 块-节点互转 helpers
// ----------------------------------------------------------------------------
// 只存放业务数据 (size + inuse 标志), 不包含链表节点 —— 节点复用
// 空闲块的用户区前 16 字节 (见 node_of/init_free)。
// 内存布局 (地址从低到高):
//   [ block_t (16B: size + inuse) | 用户可用数据区 (size 字节) ]
//   已分配: 用户区全是数据;  空闲: 用户区前 16B 是 ListNode, 其余为剩余空间。
// ============================================================================
#ifndef MMEMORY_BLOCK_H
#define MMEMORY_BLOCK_H

#include <stddef.h>  // size_t

#include "list.h"  // ListNode

namespace wageco
{
// ----------------------------------------------------------------------------
// 对齐
// ----------------------------------------------------------------------------
// 对齐粒度: 16 字节 (x86-64 上 long double / SSE 类型所需的最大对齐)
constexpr unsigned align_to = 16;
// 用 char[align_to] 保证 union 的尺寸/对齐都是 16 字节
typedef char ALIGN[align_to];

// ----------------------------------------------------------------------------
// 块头 (block)
// ----------------------------------------------------------------------------
// 只存放业务数据, 不包含链表节点 (节点复用空闲块的用户区, 见 node_of)。
// 内存布局 (地址从低到高):
//   [ block_t (16B: size + inuse) | 用户可用数据区 (size 字节) ]
//   已分配: 用户区全是数据;  空闲: 用户区前 16B 是 ListNode, 其余为剩余空间。
//   返回给用户的指针 = (block_t*)addr + 1, 即 header 之后的位置。
typedef union block
{
    struct
    {
        size_t size;  // 用户可用区大小 (不含 header 本身), 16 字节对齐
        bool inuse;   // true=已分配 (用户数据占用); false=空闲 (用户区前段是链表节点)
    } head;
    ALIGN align;  // 强制 header 尺寸/对齐为 16 字节的倍数
} block_t;

// 链表节点 <-> 宿主块头 互转。
// 节点位于"空闲块用户区的起始", 即 header 之后固定偏移 sizeof(block_t) 处,
// 因此互转只是简单指针算术 (不依赖 offsetof)。
inline ListNode* node_of(block_t* h)
{
    return reinterpret_cast<ListNode*>(reinterpret_cast<char*>(h) + sizeof(block_t));
}
inline const ListNode* node_of(const block_t* h)
{
    return reinterpret_cast<const ListNode*>(reinterpret_cast<const char*>(h) + sizeof(block_t));
}
inline block_t* block_of(ListNode* n)
{
    return reinterpret_cast<block_t*>(reinterpret_cast<char*>(n) - sizeof(block_t));
}
inline const block_t* block_of(const ListNode* n)
{
    return reinterpret_cast<const block_t*>(reinterpret_cast<const char*>(n) - sizeof(block_t));
}

// 从链表节点获取宿主块的大小 (传给 HeaderList 的 SizeFn)
inline size_t block_size_of(const ListNode* node) { return block_of(node)->head.size; }

// 初始化"已分配"块 (新申请或分割出的使用部分): 用户区全部交给数据
inline void init_allocated(block_t* node, size_t size)
{
    node->head.size = size;
    node->head.inuse = true;
}

// 初始化"空闲"块: 用户区前段复用为链表节点 (节点自环, 等待 insert)
inline void init_free(block_t* node, size_t size)
{
    node->head.size = size;
    node->head.inuse = false;
    ListNode* n = node_of(node);
    n->pre = n;
    n->next = n;
}

}  // namespace wageco
#endif  // MMEMORY_BLOCK_H
