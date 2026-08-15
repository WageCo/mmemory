// ============================================================================
// mmemory.cpp - 教学用简易内存分配器实现 (教学演示 / 逆向学习材料)
// ----------------------------------------------------------------------------
// 核心思路 (与经典 malloc 实现一脉相承):
//   1. 用 sbrk() 系统调用从操作系统申请/归还堆空间;
//   2. 每块内存前都有一个 header (元数据), 记录块大小与链表指针;
//   3. 用两条"双向循环链表"分别管理: 已分配块 (malloc_header) 与
//      空闲块 (free_header), 实现块的复用与回收。
//
// 支持的 API (位于 namespace wageco, 与标准库同名):
//   wageco::malloc / wageco::free / wageco::calloc / wageco::realloc
//
// 注意: 这是教学实现, 追求"把原理讲清楚", 不做性能优化。
//   与系统 malloc 相比, 它没有 tcache / per-thread arena 等加速机制,
//   每次分配都可能有 sbrk 系统调用, 释放时链表查找是 O(n) 的,
//   并发全靠一把全局互斥锁串行化 —— 详见 README 中的 benchmark 对比。
//
// 日志: 使用 spdlog, 默认输出到 stderr。
//   级别: DEBUG 编译默认 debug, 否则 info; 环境变量可覆盖:
//     MMEMORY_LOG_LEVEL=trace|debug|info|warn|error|critical|off
//     MMEMORY_LOG_FILE=<path>  追加模式同时写文件
// ============================================================================

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <memory>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stderr_sinks.h>

namespace wageco
{
// ----------------------------------------------------------------------------
// 日志系统 (教学示例: spdlog 的 sink 组合 + 级别配置)
//   - 默认输出到 stderr; 级别: DEBUG 编译默认 debug, 否则 info
//   - 环境变量可覆盖:
//       MMEMORY_LOG_LEVEL=trace|debug|info|warn|error|critical|off
//       MMEMORY_LOG_FILE=<path>   追加模式同时输出到文件
// ----------------------------------------------------------------------------
std::shared_ptr<spdlog::logger> get_logger()
{
    static std::shared_ptr<spdlog::logger> logger = []() {
        // sink 1: stderr 输出
        auto stderr_sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
        std::vector<spdlog::sink_ptr> sinks{stderr_sink};

        // sink 2 (可选): 追加模式文件输出
        const char *file_path = std::getenv("MMEMORY_LOG_FILE");
        if (file_path && *file_path)
        {
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(file_path, true);
            sinks.push_back(file_sink);
        }

        auto l = std::make_shared<spdlog::logger>("mmemory", sinks.begin(), sinks.end());
#ifdef DEBUG
        l->set_level(spdlog::level::debug); // 调试构建默认输出 debug 级别
#else
        l->set_level(spdlog::level::info);  // 发布构建默认 info 及以上
#endif
        const char *level_str = std::getenv("MMEMORY_LOG_LEVEL");
        if (level_str && *level_str)
        {
            l->set_level(spdlog::level::from_str(level_str));
        }
        l->flush_on(spdlog::level::info); // 教学: info 及以上立即落盘
        return l;
    }();
    return logger;
}
// ----------------------------------------------------------------------------
// 块头 (header) 与对齐
// ----------------------------------------------------------------------------
// 对齐粒度: 16 字节 (x86-64 上 long double / SSE 类型所需的最大对齐)
const unsigned align_to = 16;
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

// 全局分配器状态 (静态存储, 零初始化: 两个链表头为 NULL, 计数器为 0)
static stack_memory_t stack_memory_list;

// 多线程防竞争: 所有链表操作/brk 操作都在这把全局锁内完成。
// 简单可靠, 但并发场景会串行化 —— 教学取舍。
static pthread_mutex_t list_locker = PTHREAD_MUTEX_INITIALIZER;

// ----------------------------------------------------------------------------
// 双向循环链表基础操作
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

// ----------------------------------------------------------------------------
// custom malloc
// ----------------------------------------------------------------------------
// 流程:
//   1. 需求大小按 16 字节对齐 (总占用 = header + 用户区, 向上取整到 16 倍数);
//   2. 先在空闲链表中 first-fit 找一块够大的;
//        - 命中: 若块过大则 split 分割, 剩余部分放回空闲链表, 然后取用;
//        - 未命中: 调用 sbrk() 向操作系统要新的堆空间;
//   3. 把块挂到已分配链表, 返回用户区指针 (header 之后)。
void *malloc(size_t size)
{
    if (!size)
    {
        return nullptr; // 语义: malloc(0) 允许返回 NULL
    }
    pthread_mutex_lock(&list_locker);
    // 总占用 = header(16) + 用户 size, 向上对齐到 16 的倍数
    // 例如: size=1   -> total = (16+1+15)&~15 = 32
    //        size=500 -> total = (16+500+15)&~15 = 528
    size_t total_size = (sizeof(header_t) + size + align_to - 1) & ~(align_to - 1);
    // search list_free: 优先复用已释放的块
    if (stack_memory_list.free_header)
    {
        header_t *node = list_find(stack_memory_list.free_header, total_size - sizeof(header_t));
        if (node)
        {
            // 命中空闲块。块比需要的大时, 切出剩余部分 (split) 放回空闲链表,
            // 避免大块被小块占住导致碎片。
            size_t need_size = total_size - sizeof(header_t);
            if (node->head.size >= need_size + sizeof(header_t) + align_to)
            {
                // 剩余部分还能构成一个"至少 16 字节用户区"的完整块才分割
                header_t *rest = (header_t *)((char *)(node + 1) + need_size);
                list_init(rest, node->head.size - need_size - sizeof(header_t));
                list_insert(stack_memory_list.free_header, rest);
                ++stack_memory_list.free_size;
                node->head.size = need_size; // 取用部分缩小为正好需要的大小
            }
            // 从空闲链表摘除 -> 挂到已分配链表
            list_delete(stack_memory_list.free_header, node);
            --stack_memory_list.free_size;
            list_insert(stack_memory_list.malloc_header, node);
            ++stack_memory_list.malloc_size;
            pthread_mutex_unlock(&list_locker);
            return (void *)(node + 1); // 跳过 header, 返回用户区
        }
    }
    // 空闲链表没有合适块: 通过 sbrk 向操作系统申请新内存
    // (sbrk 把"程序断点" program break 向上移动, 返回旧断点地址)
    void *now_addr = sbrk(total_size);
    // failed
    if (now_addr == (void *)-1)
    {
        get_logger()->error("malloc failed, errno: {} ({})", errno, strerror(errno));
        pthread_mutex_unlock(&list_locker);
        return nullptr;
    }
    // success: 新块初始化并挂到已分配链表
    header_t *node = (header_t *)now_addr;
    list_init(node, total_size - sizeof(header_t));
    list_insert(stack_memory_list.malloc_header, node);
    ++stack_memory_list.malloc_size;
    pthread_mutex_unlock(&list_locker);
    get_logger()->debug("malloc: alloc {} bytes (user {}), start: {:p}", total_size, size, (void *)sbrk(0));
    // remove header: 用户只看到 header 之后的区域
    return (void *)(node + 1);
}

// ----------------------------------------------------------------------------
// custom free
// ----------------------------------------------------------------------------
// 流程:
//   1. 由用户指针回退得到 header, 校验合法性 (防 double free / 释放未分配内存);
//   2. 从已分配链表摘除;
//   3. 合并物理相邻的空闲块 (前驱 pred / 后继 succ), 缓解碎片;
//   4. 若合并后的块位于堆顶 (program break 处), 把整段 (含其下方相邻的
//      空闲块链) 通过 sbrk(-) 归还操作系统; 否则挂回空闲链表待复用。
void free(void *addr)
{
    if (!addr)
    {
        return; // free(NULL) 是合法空操作
    }
    header_t *node = (header_t *)addr - 1; // 回退 16 字节拿到 header
    pthread_mutex_lock(&list_locker);
    // 合法性校验: 已在空闲链表 (double free) 或不在已分配链表 (非法指针)
    if (list_find(stack_memory_list.free_header, node) || !list_find(stack_memory_list.malloc_header, node))
    {
        get_logger()->error("free: double free or no malloc, start: {:p}", (void *)node);
        pthread_mutex_unlock(&list_locker);
        return;
    }
    void *program_break_now = sbrk(0); // 记录当前堆顶, 供后续判断是否可收缩
    list_delete(stack_memory_list.malloc_header, node);
    --stack_memory_list.malloc_size;

    // --- 合并 (coalescing): 把物理相邻的空闲块并成一块 ---
    // 1) 后继合并: node 后面紧邻一块空闲块, 则把它并入 node
    header_t *succ = list_find_next(stack_memory_list.free_header, node);
    if (succ)
    {
        list_delete(stack_memory_list.free_header, succ);
        --stack_memory_list.free_size;
        node->head.size += sizeof(header_t) + succ->head.size;
    }
    // 2) 前驱合并: node 前面紧邻一块空闲块, 则把 node 并入前驱
    header_t *pred = list_find_prev(stack_memory_list.free_header, node);
    if (pred)
    {
        list_delete(stack_memory_list.free_header, pred);
        pred->head.size += sizeof(header_t) + node->head.size;
        node = pred; // 合并后的块用前驱表示
        --stack_memory_list.free_size;
    }

    // --- 堆顶收缩: 能还就还给操作系统 ---
    // 若合并后的块物理末尾 == program break, 说明它是堆上最后一块,
    // 可以直接 sbrk(-) 把空间归还 (这部分内存"零成本"回收)。
    if ((char *)node + sizeof(header_t) + node->head.size == program_break_now)
    {
        // 连带回收其下方"物理相邻且已释放"的空闲块链:
        // 例如下方还有 2 个连续空闲块, 一并归还, 避免它们滞留堆中。
        size_t reclaim = sizeof(header_t) + node->head.size;
        header_t *p = node;
        for (;;)
        {
            header_t *prev = list_find_prev(stack_memory_list.free_header, p);
            if (!prev)
            {
                break;
            }
            list_delete(stack_memory_list.free_header, prev);
            --stack_memory_list.free_size;
            reclaim += sizeof(header_t) + prev->head.size;
            p = prev;
        }
        // 回收链整体视为一块: 更新最底块的 size (供收缩失败时放回空闲链表)
        p->head.size = reclaim - sizeof(header_t);
        get_logger()->debug("free: reclaim {} bytes, start: {:p}", reclaim, (void *)sbrk(0));
        // free memory space: sbrk 传负数表示把断点下移 (归还内存)
        if (sbrk(0 - reclaim) == (void *)-1)
        {
            get_logger()->error("free: sbrk shrink failed, errno: {} ({})", ENOMEM, strerror(ENOMEM));
            // 归还失败: 把整条回收链放回空闲链表, 避免内存丢失
            list_insert(stack_memory_list.free_header, p);
            ++stack_memory_list.free_size;
            pthread_mutex_unlock(&list_locker);
            return;
        }
    }
    else
    {
        // 不是堆顶块: 挂回空闲链表, 等后续 malloc 复用 / 合并
        list_insert(stack_memory_list.free_header, node);
        ++stack_memory_list.free_size;
    }

    if (stack_memory_list.malloc_size == 0 && stack_memory_list.free_size > 0)
    {
        // TODO: 理论上全部释放后可以把整堆归还; 现在依赖上面的"堆顶连带回收"
        //       已经能覆盖绝大多数情况, 这里留作扩展点。
    }
    pthread_mutex_unlock(&list_locker);
}

// ----------------------------------------------------------------------------
// custom calloc = malloc + 清零
// ----------------------------------------------------------------------------
void *calloc(size_t num, size_t size)
{
    if (!num || !size)
    {
        return nullptr;
    }
    // 溢出检查: num * size 可能超过 size_t 上限
    if (num > (size_t)-1 / size)
    {
        return nullptr;
    }
    size_t total_size = num * size;
    void *addr = malloc(total_size);
    if (!addr)
    {
        return nullptr;
    }
    memset(addr, 0, total_size); // 置零
    return addr;
}

// ----------------------------------------------------------------------------
// custom realloc
// ----------------------------------------------------------------------------
// 语义 (与标准 realloc 对齐):
//   - realloc(ptr, 0)   -> 释放 ptr 并返回 NULL;
//   - realloc(NULL, n)  -> 等价 malloc(n);
//   - 原块够大          -> 原地返回, 不动数据;
//   - 原块不够大        -> 新分配 + 拷贝旧数据 + 释放旧块。
void *realloc(void *addr, size_t size)
{
    if (!size)
    {
        // 标准语义: 释放并返回 NULL (历史版本漏了 free, 会泄漏)
        free(addr);
        return nullptr;
    }
    if (!addr)
    {
        return malloc(size);
    }
    header_t *node = (header_t *)addr - 1;
    if (node->head.size >= size)
    {
        return addr; // 原地满足, 直接返回 (不分割, 教学取舍)
    }
    // 需要扩容: 重新分配, 拷贝旧数据, 释放旧块
    void *new_addr = malloc(size);
    if (new_addr)
    {
        // 只拷贝旧块实际可用的大小, 避免越界读
        memcpy(new_addr, addr, node->head.size);
        free(addr);
    }
    return new_addr;
}
} // namespace wageco
