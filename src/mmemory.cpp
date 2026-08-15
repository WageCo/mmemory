// ============================================================================
// mmemory.cpp - 教学用简易内存分配器: 核心 API 实现
// ----------------------------------------------------------------------------
// 模块结构 (src/):
//   internal.h  - 内部共享: 块头 header_t / HeaderList 链表类 (含节点计数)
//                 / 日志配置 (SPDLOG_ACTIVE_LEVEL) 与 get_logger 声明
//   log.cpp     - 日志系统实现 (spdlog 封装, 非热路径)
//   mmemory.cpp - 全局分配器状态 + malloc/free/calloc/realloc
//
// 核心思路 (与经典 malloc 实现一脉相承):
//   1. 用 sbrk() 系统调用从操作系统申请/归还堆空间;
//   2. 每块内存前都有一个 header (元数据), 记录块大小与链表指针;
//   3. 用两个 HeaderList 分别管理: 已分配块 (malloc_list) 与
//      空闲块 (free_list), 实现块的复用与回收。
//
// 支持的 API (位于 namespace wageco, 与标准库同名):
//   wageco::malloc / wageco::free / wageco::calloc / wageco::realloc
//
// 注意: 这是教学实现, 追求"把原理讲清楚", 不做性能优化。
//   与系统 malloc 相比, 它没有 tcache / per-thread arena 等加速机制,
//   每次分配都可能有 sbrk 系统调用, 释放时链表查找是 O(n) 的,
//   并发全靠一把全局互斥锁串行化 —— 详见 README 中的 benchmark 对比。
//
// 日志: 使用 spdlog (见 internal.h / log.cpp)。
//   默认 stderr 输出; 级别: DEBUG 编译默认 debug, 否则 info; 环境变量可覆盖:
//     MMEMORY_LOG_LEVEL=trace|debug|info|warn|error|critical|off
//     MMEMORY_LOG_FILE=<path>  指定则改为写文件 (追加), 否则 stderr
// ============================================================================

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

namespace wageco
{
// ----------------------------------------------------------------------------
// 分配器全局状态
// ----------------------------------------------------------------------------
// 已分配块链表 / 空闲块链表 (静态存储, 零初始化; 节点计数由 HeaderList 内部维护)
static HeaderList malloc_list;
static HeaderList free_list;

// 多线程防竞争: 所有链表操作/brk 操作都在这把全局锁内完成。
// 简单可靠, 但并发场景会串行化 —— 教学取舍。
static pthread_mutex_t list_locker = PTHREAD_MUTEX_INITIALIZER;

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
    SPDLOG_LOGGER_DEBUG(get_logger(), "malloc: request {} bytes (total {} with header)", size, total_size);
    // search free list: 优先复用已释放的块
    header_t *node = free_list.find_first_fit(total_size - sizeof(header_t));
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
            free_list.insert(rest);
            node->head.size = need_size; // 取用部分缩小为正好需要的大小
            SPDLOG_LOGGER_TRACE(get_logger(), "malloc: split free block, remainder {} bytes -> free list", rest->head.size);
        }
        // 从空闲链表摘除 -> 挂到已分配链表 (计数由 HeaderList 自动维护)
        free_list.remove(node);
        malloc_list.insert(node);
        SPDLOG_LOGGER_DEBUG(get_logger(), "malloc: reuse free block, start: {:p} (user {} bytes)", (void *)(node + 1), node->head.size);
        pthread_mutex_unlock(&list_locker);
        return (void *)(node + 1); // 跳过 header, 返回用户区
    }
    // 空闲链表没有合适块: 通过 sbrk 向操作系统申请新内存
    // (sbrk 把"程序断点" program break 向上移动, 返回旧断点地址)
    void *now_addr = sbrk(total_size);
    // failed
    if (now_addr == (void *)-1)
    {
        SPDLOG_LOGGER_ERROR(get_logger(), "malloc failed, errno: {} ({})", errno, strerror(errno));
        pthread_mutex_unlock(&list_locker);
        return nullptr;
    }
    // success: 新块初始化并挂到已分配链表
    node = (header_t *)now_addr;
    list_init(node, total_size - sizeof(header_t));
    malloc_list.insert(node);
    pthread_mutex_unlock(&list_locker);
    SPDLOG_LOGGER_DEBUG(get_logger(), "malloc: sbrk new block, total {} bytes (user {}), start: {:p}", total_size, size, (void *)sbrk(0));
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
    if (free_list.contains(node) || !malloc_list.contains(node))
    {
        SPDLOG_LOGGER_ERROR(get_logger(), "free: double free or no malloc, start: {:p}", (void *)node);
        pthread_mutex_unlock(&list_locker);
        return;
    }
    SPDLOG_LOGGER_DEBUG(get_logger(), "free: addr {:p} -> block {:p} (user {} bytes)", addr, (void *)node, node->head.size);
    void *program_break_now = sbrk(0); // 记录当前堆顶, 供后续判断是否可收缩
    malloc_list.remove(node);

    // --- 合并 (coalescing): 把物理相邻的空闲块并成一块 ---
    // 1) 后继合并: node 后面紧邻一块空闲块, 则把它并入 node
    header_t *succ = free_list.find_next_phys(node);
    if (succ)
    {
        free_list.remove(succ);
        node->head.size += sizeof(header_t) + succ->head.size;
        SPDLOG_LOGGER_TRACE(get_logger(), "free: coalesce next block {:p}, block now {} bytes", (void *)succ, node->head.size);
    }
    // 2) 前驱合并: node 前面紧邻一块空闲块, 则把 node 并入前驱
    header_t *pred = free_list.find_prev_phys(node);
    if (pred)
    {
        free_list.remove(pred);
        pred->head.size += sizeof(header_t) + node->head.size;
        node = pred; // 合并后的块用前驱表示
        SPDLOG_LOGGER_TRACE(get_logger(), "free: coalesce into prev block {:p}, block now {} bytes", (void *)node, node->head.size);
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
            header_t *prev = free_list.find_prev_phys(p);
            if (!prev)
            {
                break;
            }
            free_list.remove(prev);
            reclaim += sizeof(header_t) + prev->head.size;
            SPDLOG_LOGGER_TRACE(get_logger(), "free: reclaim contiguous prev block {:p} ({} bytes)", (void *)prev, prev->head.size);
            p = prev;
        }
        // 回收链整体视为一块: 更新最底块的 size (供收缩失败时放回空闲链表)
        p->head.size = reclaim - sizeof(header_t);
        SPDLOG_LOGGER_DEBUG(get_logger(), "free: reclaim {} bytes, start: {:p}", reclaim, (void *)sbrk(0));
        // free memory space: sbrk 传负数表示把断点下移 (归还内存)
        if (sbrk(0 - reclaim) == (void *)-1)
        {
            SPDLOG_LOGGER_ERROR(get_logger(), "free: sbrk shrink failed, errno: {} ({})", ENOMEM, strerror(ENOMEM));
            // 归还失败: 把整条回收链放回空闲链表, 避免内存丢失
            free_list.insert(p);
            pthread_mutex_unlock(&list_locker);
            return;
        }
    }
    else
    {
        // 不是堆顶块: 挂回空闲链表, 等后续 malloc 复用 / 合并
        free_list.insert(node);
        SPDLOG_LOGGER_TRACE(get_logger(), "free: block {:p} (user {} bytes) -> free list", (void *)(node + 1), node->head.size);
    }

    if (malloc_list.empty() && !free_list.empty())
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
        SPDLOG_LOGGER_ERROR(get_logger(), "calloc: size overflow ({} x {})", num, size);
        return nullptr;
    }
    size_t total_size = num * size;
    void *addr = malloc(total_size);
    if (!addr)
    {
        return nullptr;
    }
    memset(addr, 0, total_size); // 置零
    SPDLOG_LOGGER_TRACE(get_logger(), "calloc: {} x {} = {} bytes, zeroed", num, size, total_size);
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
        SPDLOG_LOGGER_DEBUG(get_logger(), "realloc: size 0, freeing {:p}", addr);
        free(addr);
        return nullptr;
    }
    if (!addr)
    {
        SPDLOG_LOGGER_DEBUG(get_logger(), "realloc: NULL addr, malloc {} bytes", size);
        return malloc(size);
    }
    header_t *node = (header_t *)addr - 1;
    if (node->head.size >= size)
    {
        SPDLOG_LOGGER_TRACE(get_logger(), "realloc: in-place, old {} >= new {}", node->head.size, size);
        return addr; // 原地满足, 直接返回 (不分割, 教学取舍)
    }
    // 需要扩容: 重新分配, 拷贝旧数据, 释放旧块
    SPDLOG_LOGGER_DEBUG(get_logger(), "realloc: growing {:p} from {} to {} bytes", addr, node->head.size, size);
    void *new_addr = malloc(size);
    if (new_addr)
    {
        // 只拷贝旧块实际可用的大小, 避免越界读
        memcpy(new_addr, addr, node->head.size);
        free(addr);
        SPDLOG_LOGGER_TRACE(get_logger(), "realloc: moved to {:p} (copied {} bytes)", new_addr, node->head.size);
    }
    return new_addr;
}
} // namespace wageco
