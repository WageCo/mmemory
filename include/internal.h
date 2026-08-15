// ============================================================================
// internal.h - mmemory 内部共享定义 (块头 + 链表类 + 日志配置)
// ----------------------------------------------------------------------------
// 注意: 本头属于"内部实现细节", 不是对外公共接口 (对外接口见 mmemory.h);
//       头文件统一放在 include/ 下管理, 便于统一索引与工具链处理。
//
// 解耦设计:
//   - HeaderList 定义自己的链表节点 Node (只含 pre/next 链接指针);
//   - 分配器的块头 header_t "嵌入" HeaderList::Node —— 链表只负责链接,
//     块大小等业务数据由宿主 (header_t) 提供;
//   - HeaderList 通过 SizeFn 回调获取节点对应宿主块的大小, 通过
//     header_of()/node_of() 在"节点"与"宿主块"之间转换 (嵌入节点手法,
//     与 dlmalloc 等真实分配器一致)。
// ============================================================================
#ifndef MMEMORY_INTERNAL_H
#define MMEMORY_INTERNAL_H

#include <stddef.h> // size_t, offsetof
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
// 对齐
// ----------------------------------------------------------------------------
// 对齐粒度: 16 字节 (x86-64 上 long double / SSE 类型所需的最大对齐)
constexpr unsigned align_to = 16;
// 用 char[align_to] 保证 union 的尺寸/对齐都是 16 字节
typedef char ALIGN[align_to];

// ----------------------------------------------------------------------------
// HeaderList - 管理内存块的双向循环链表 (类封装, 便于扩展)
// ----------------------------------------------------------------------------
// 职责:
//   - 定义链表节点 Node (只含链接指针, 与块大小等业务数据解耦);
//   - 维护循环链表 (头指针) 与节点计数 count, insert/remove 自动维护计数;
//   - 提供查找: first-fit (按大小) / contains (按指针) / 物理相邻 (合并用);
//   - 块大小的获取通过 SizeFn 回调 (由分配器提供), 链表不关心数据如何存放;
//   - 扩展点: 以后可按大小分 bin (一个分配器持有多个 HeaderList)、
//     增加字节总量统计等, 都只需在此类上做增量修改。
//
// 实现位置: 定义在 src/list.cpp (声明/实现分离); 简单访问器保留内联。
// 性能注记: 非内联引入的函数调用开销 (~ns 级) 相对分配器的主要成本
//   (全局锁 + sbrk 系统调用, 单次 ~µs 级) 可忽略, benchmark 已佐证。
class HeaderList
{
public:
    // 链表节点: 只负责链接。宿主块通过"嵌入"持有它 (见 header_t)。
    struct Node
    {
        Node *pre;  // 前驱
        Node *next; // 后继
    };

    // 从链表节点获取其宿主块大小的回调 (解耦的关键: 链表不直接认识宿主类型)
    using SizeFn = size_t (*)(const Node *node);

    explicit HeaderList(SizeFn size_fn) : head_(nullptr), count_(0), size_fn_(size_fn) {}

    // 是否为空
    bool empty() const { return head_ == nullptr; }

    // 当前节点数
    size_t size() const { return count_; }

    // 把 node 插入链表头部 (node 需先经 list_init 初始化)。
    // 循环链表不需要尾指针: 头的前驱即尾。实现见 src/list.cpp。
    void insert(Node *node);

    // first-fit: 返回第一个"宿主块大小 >= size"的节点 (只查找, 不摘除)。
    // (历史版本是精确匹配 == size, 空闲块几乎无法复用, 造成碎片泄漏;
    //  改为 first-fit 后配合 malloc 里的 split, 空闲块才能被充分复用)
    // 实现见 src/list.cpp。
    Node *find_first_fit(size_t size) const;

    // 判断 node 是否在链表中 (用于 free 的合法性校验)。实现见 src/list.cpp。
    bool contains(Node *node) const;

    // 物理地址紧邻 node 之前的节点 (释放时向前合并用)。实现见 src/list.cpp。
    Node *find_prev_phys(Node *node) const;

    // 物理地址紧邻 node 之后的节点 (释放时向后合并用)。实现见 src/list.cpp。
    Node *find_next_phys(Node *node) const;

    // 摘除 node (调用方需保证 node 在链表中), 并把 node 恢复为孤立节点。
    // 计数自动 -1。实现见 src/list.cpp。
    void remove(Node *node);

private:
    Node *head_;    // 循环链表头 (nullptr 表示空表)
    size_t count_;  // 节点数
    SizeFn size_fn_; // 获取节点对应宿主块大小的回调
};

// ----------------------------------------------------------------------------
// 块头 (header) 与对齐
// ----------------------------------------------------------------------------
// 每个内存块的最前面都是这个 header, 其中"嵌入"了链表的节点 (Headlist::Node):
//   [ node (16B: pre/next) | size (8B) | 用户可用数据区 ]
// 内存布局 (地址从低到高):
//   [ header_t (24B) | 用户可用数据区 (size 字节) ]
//   返回给用户的指针 = (header_t*)addr + 1, 即 header 之后的位置。
typedef union header {
    struct
    {
        HeaderList::Node node; // 链表节点 (嵌入, 由 HeaderList 管理链接)
        size_t size;           // 用户可用区大小 (不含 header 本身), 16 字节对齐
    } head;
    ALIGN align;               // 强制 header 尺寸/对齐为 16 字节的倍数
} header_t;

// 链表节点 <-> 宿主块头 互转 (嵌入节点手法: 用 offsetof 反推宿主起始地址)
inline HeaderList::Node *node_of(header_t *h)
{
    return &h->head.node;
}
inline const HeaderList::Node *node_of(const header_t *h)
{
    return &h->head.node;
}
inline header_t *header_of(HeaderList::Node *n)
{
    return reinterpret_cast<header_t *>(reinterpret_cast<char *>(n) - offsetof(header_t, head.node));
}
inline const header_t *header_of(const HeaderList::Node *n)
{
    return reinterpret_cast<const header_t *>(reinterpret_cast<const char *>(n) - offsetof(header_t, head.node));
}

// 从链表节点获取宿主块的大小 (传给 HeaderList 的 SizeFn)
inline size_t block_size_of(const HeaderList::Node *node)
{
    return header_of(node)->head.size;
}

// 把块头初始化为"只有自己一个节点"的孤立状态, 并记录大小。
// (这是对"块"本身的初始化, 不属于链表类职责, 故保留为自由函数)
inline void list_init(header_t *node, size_t size)
{
    node->head.node.pre = &node->head.node;
    node->head.node.next = &node->head.node;
    node->head.size = size;
}

} // namespace wageco
#endif // MMEMORY_INTERNAL_H
