// ============================================================================
// internal.h - mmemory 内部共享定义 (数据结构 + 链表类 + 日志配置)
// ----------------------------------------------------------------------------
// 注意: 本头属于"内部实现细节", 不是对外公共接口 (对外接口见 mmemory.h);
//       头文件统一放在 include/ 下管理, 便于统一索引与工具链处理。
// 链表用 HeaderList 类封装: 职责内聚 (链表维护 + 节点计数), 便于后续扩展
// (例如: 把空闲表改成按大小分 bin 的多个链表、增加统计字段等)。
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

// 把 node 初始化为"只有自己一个节点"的循环链表, 并记录大小。
// (这是对"块"本身的初始化, 不属于链表类职责, 故保留为自由函数)
inline void list_init(header_t *node, size_t size)
{
    node->head.pre = node;
    node->head.next = node;
    node->head.size = size;
}

// ----------------------------------------------------------------------------
// HeaderList - 管理 header_t 块的双向循环链表 (类封装, 便于扩展)
// ----------------------------------------------------------------------------
// 职责:
//   - 维护循环链表 (头指针) 与节点计数 count, insert/remove 自动维护计数;
//   - 提供查找: first-fit (按大小) / contains (按指针) / 物理相邻 (合并用);
//   - 扩展点: 以后可按大小分 bin (一个分配器持有多个 HeaderList)、
//     增加字节总量统计、按地址有序插入等, 都只需在此类上做增量修改。
//
// 实现位置: 复杂操作 (insert/remove/find*) 定义在 src/list.cpp (声明/实现分离,
//   标准 C++ 布局); 简单访问器 (empty/size) 保留内联。
// 性能注记: 非内联引入的函数调用开销 (~ns 级) 相对分配器的主要成本
//   (全局锁 + sbrk 系统调用, 单次 ~µs 级) 可忽略, benchmark 已佐证;
//   仅当未来把链表替换为更低开销结构时才需重新考虑内联/LTO。
class HeaderList
{
public:
    HeaderList() : head_(nullptr), count_(0) {}

    // 是否为空
    bool empty() const { return head_ == nullptr; }

    // 当前节点数
    size_t size() const { return count_; }

    // 把 node 插入链表头部 (node 需先经 list_init 初始化)。
    // 循环链表不需要尾指针: 头的前驱即尾。实现见 src/list.cpp。
    void insert(header_t *node);

    // first-fit: 返回第一个"用户可用区 >= size"的块 (只查找, 不摘除)。
    // (历史版本是精确匹配 == size, 空闲块几乎无法复用, 造成碎片泄漏;
    //  改为 first-fit 后配合 malloc 里的 split, 空闲块才能被充分复用)
    // 实现见 src/list.cpp。
    header_t *find_first_fit(size_t size) const;

    // 判断 node 是否在链表中 (用于 free 的合法性校验)。实现见 src/list.cpp。
    bool contains(header_t *node) const;

    // 物理地址紧邻 node 之前的块 (释放时向前合并用)。实现见 src/list.cpp。
    header_t *find_prev_phys(header_t *node) const;

    // 物理地址紧邻 node 之后的块 (释放时向后合并用)。实现见 src/list.cpp。
    header_t *find_next_phys(header_t *node) const;

    // 摘除 node (调用方需保证 node 在链表中), 并把 node 恢复为孤立节点。
    // 计数自动 -1。实现见 src/list.cpp。
    void remove(header_t *node);

private:
    header_t *head_; // 循环链表头 (nullptr 表示空表)
    size_t count_;   // 节点数
};

} // namespace wageco
#endif // MMEMORY_INTERNAL_H
