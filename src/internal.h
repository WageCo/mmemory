// ============================================================================
// internal.h - mmemory 内部共享定义 (数据结构 + inline 链表 + 日志配置)
// ----------------------------------------------------------------------------
// 仅供库内部使用 (src/*.cpp), 不对外暴露。
// 链表操作实现为 inline 函数: 它们在 malloc/free 的热路径中被频繁调用,
// 放在头文件里可以让编译器内联, 避免函数调用开销 (与 benchmark 相关)。
// ============================================================================
#ifndef MMEMORY_INTERNAL_H
#define MMEMORY_INTERNAL_H

#include <stddef.h> // size_t
#include <memory>   // std::shared_ptr

// 编译期日志级别: DEBUG 构建保留 debug/trace; Release 构建剥离 debug/trace
// (SPDLOG_ACTIVE_LEVEL 使 SPDLOG_LOGGER_DEBUG/TRACE 宏在编译期展开为空,
//  参数不求值、零开销 —— 保证 benchmark 测的是纯分配器逻辑, 不被日志污染)
#ifdef DEBUG
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#else
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#endif
#include <spdlog/spdlog.h>
// 注意: spdlog 1.17.0 中 stderr_logger_mt 声明在 stdout_sinks.h,
//       basic_logger_mt 声明在 basic_file_sink.h (不存在 stderr_sinks.h)
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace wageco
{
// 日志: 定义在 src/log.cpp
std::shared_ptr<spdlog::logger> get_logger();

// ----------------------------------------------------------------------------
// 块头 (header) 与对齐
// ----------------------------------------------------------------------------
// 对齐粒度: 16 字节 (x86-64 上 long double / SSE 类型所需的最大对齐)
constexpr unsigned align_to = 16;
// 用 char[align_to] 保证 union 的尺寸/对齐都是 16 字节
typedef char ALIGN[align_to];

// 每个内存块的最前面都是这个 header。
// union 的两种视角:
//   - head: 元数据 (size + 双向链表指针), 链表操作时使用;
//   - align: 强制整个 header 为 16 字节, 从而保证用户数据区也 16 字节对齐。
// 内存布局 (地址从低到高):
//   [ header_t (16B) | 用户可用数据区 (size 字节) ]
//   返回给用户的指针 = (header_t*)addr + 1, 即 header 之后的位置。
typedef union header {
    struct
    {
        size_t size;        // 用户可用区大小 (不含 header 本身), 16 字节对齐
        union header *pre;  // 双向链表前驱
        union header *next; // 双向链表后继
    } head;
    ALIGN align;
} header_t;

// ----------------------------------------------------------------------------
// 分配器全局状态
// ----------------------------------------------------------------------------
typedef struct
{
    header_t *malloc_header; // "已分配块" 循环链表头 (NULL 表示空表)
    header_t *free_header;   // "空闲块" 循环链表头
    size_t malloc_size;      // 已分配块个数
    size_t free_size;        // 空闲块个数
} stack_memory_t;

// ----------------------------------------------------------------------------
// 双向循环链表基础操作 (inline: 热路径, 见文件头说明)
// ----------------------------------------------------------------------------

// 把 node 初始化为"只有自己一个节点"的循环链表, 并记录大小
inline void list_init(header_t *node, size_t size)
{
    node->head.pre = node;
    node->head.next = node;
    node->head.size = size;
}

// 把 node 插入链表头部 (header 是链表头的引用, 插入后 node 成为新头)。
// 循环链表不需要尾指针: 头的前驱即尾。
inline void list_insert(header_t *&header, header_t *node)
{
    if (header)
    {
        // 非空表: 在头节点之前插入 (即链表尾部), 并更新头
        header->head.pre->head.next = node; // 尾->next = node
        node->head.pre = header->head.pre;  // node->pre = 原尾
        node->head.next = header;           // node->next = 原头
        header->head.pre = node;            // 原头->pre = node
    }
    header = node; // 空表时 node 自成环, 成为头
}

// 按大小查找: first-fit 策略 —— 返回第一个"用户可用区 >= size"的块。
// (历史版本是精确匹配 == size, 空闲块几乎无法复用, 造成碎片泄漏;
//  改为 first-fit 后配合 malloc 里的 split, 空闲块才能被充分复用)
inline header_t *list_find(header_t *header, size_t size)
{
    if (header)
    {
        header_t *node = header;
        // 从头遍历, 跳过所有 size 不足的块; 走到头即停 (循环链表判空)
        for (; node->head.size < size && node->head.next != header; node = node->head.next)
            ;
        if (node->head.size >= size)
        {
            return node;
        }
    }
    return nullptr;
}

// 按指针查找: 判断 node 是否在链表中 (用于 free 的合法性校验)
inline bool list_find(header_t *header, header_t *node)
{
    if (header)
    {
        header_t *p = header;
        for (; p != node && p->head.next != header; p = p->head.next)
            ;
        if (p == node)
        {
            return true;
        }
    }
    return false;
}

// 在链表中查找"物理地址紧邻 node 之前"的块。
// 物理相邻判断: 块 p 的末尾 (header + 用户区末尾) 正好是 node 的起始地址。
// 用于释放时的向前合并 (coalescing)。
inline header_t *list_find_prev(header_t *header, header_t *node)
{
    if (header)
    {
        header_t *p = header;
        do
        {
            if ((char *)p + sizeof(header_t) + p->head.size == (char *)node)
            {
                return p;
            }
            p = p->head.next;
        } while (p != header);
    }
    return nullptr;
}

// 在链表中查找"物理地址紧邻 node 之后"的块。
// 判断: 某块 q 的起始地址 == node 的末尾地址。
// 用于释放时的向后合并。
inline header_t *list_find_next(header_t *header, header_t *node)
{
    if (header)
    {
        char *node_end = (char *)node + sizeof(header_t) + node->head.size;
        header_t *q = header;
        do
        {
            if ((char *)q == node_end)
            {
                return q;
            }
            q = q->head.next;
        } while (q != header);
    }
    return nullptr;
}

// 从链表摘除 node (header 是链表头的引用, 摘除后可能更新头)。
// 摘除后 node 重新变成孤立节点 (list_init), 便于后续重新挂到别的链表。
inline void list_delete(header_t *&header, header_t *node)
{
    if (header && header->head.next != header)
    {
        // 多于一个节点: 普通摘除
        if (node == header)
        {
            header = header->head.next; // 摘的是头, 则后移头指针
        }
        node->head.pre->head.next = node->head.next; // 前驱跳过 node
        node->head.next->head.pre = node->head.pre;  // 后继跳过 node
        list_init(node, node->head.size);            // node 变回孤立节点
    }
    else
    {
        // 链表只剩这一个节点: 摘除后整表为空
        header = nullptr;
    }
}

} // namespace wageco
#endif // MMEMORY_INTERNAL_H
