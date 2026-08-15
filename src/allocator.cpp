// ============================================================================
// allocator.cpp - Allocator 核心分配逻辑实现
// ----------------------------------------------------------------------------
// 类声明在 include/internal.h (声明/实现分离, 标准 C++ 布局)。
// 职责: malloc / free / calloc / realloc 四个核心操作的完整实现。
// 设计要点:
//   - 只维护"空闲链表" (free_list_, 注入的 IList);
//   - 块是否已分配由 block_t 的 inuse 标志标识 (无"已分配链表");
//   - 释放完全委托给内存提供者 (IMemory), 分配器不含任何栈式回收代码:
//       supports_random_release() 为 true  → 直接归还 (随机释放);
//       false (如 sbrk)                   → 按"申请顺序反向释放":
//         release_block 仅对最后申请的块成功, 成功后连带其下方紧邻的
//         空闲块继续反向归还; 失败则挂回空闲链表复用。
// 全局实例与对外 API 转发见 src/mmemory.cpp。
// ============================================================================
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

namespace wageco
{
// ----------------------------------------------------------------------------
// Allocator::malloc
// ----------------------------------------------------------------------------
// 流程:
//   1. 需求大小按 16 字节对齐 (总占用 = block_t + 用户区, 向上取整到 16 倍数);
//   2. 先在空闲链表中 first-fit 找一块够大的;
//        - 命中: 若块过大则 split 分割, 剩余部分放回空闲链表, 然后取用;
//        - 未命中: 通过内存提供者 (IMemory) 申请新堆空间;
//   3. 把块标记为已分配 (inuse=true), 返回用户区指针 (block_t 之后)。
void* Allocator::malloc(size_t size)
{
    if (!size)
    {
        return nullptr;  // 语义: malloc(0) 允许返回 NULL
    }
    // 防护: 总占用计算 (block + size + 对齐) 不得溢出 size_t, 否则回绕
    // 会算出极小值导致错误分配
    if (size > (size_t)-1 - sizeof(block_t) - align_to)
    {
        SPDLOG_LOGGER_ERROR(get_logger(), "malloc: request too large ({} bytes)", size);
        return nullptr;
    }
    pthread_mutex_lock(&locker_);
    // 总占用 = block_t(16) + 用户 size, 向上对齐到 16 的倍数
    // 例如: size=1   -> total = (16+1+15)&~15 = 32
    //        size=500 -> total = (16+500+15)&~15 = 528
    size_t total_size = (sizeof(block_t) + size + align_to - 1) & ~(align_to - 1);
    SPDLOG_LOGGER_DEBUG(get_logger(), "malloc: request {} bytes (total {} with block header)", size, total_size);
    // search free list: 按注入的查找策略找可用块 (链表返回节点, 转回宿主块)
    ListNode* free_node = strategy_->find(free_list_, total_size - sizeof(block_t));
    if (free_node)
    {
        block_t* node = block_of(free_node);
        // 命中空闲块。块比需要的大时, 切出剩余部分 (split) 放回空闲链表,
        // 避免大块被小块占住导致碎片。
        size_t need_size = total_size - sizeof(block_t);
        if (node->head.size >= need_size + sizeof(block_t) + align_to)
        {
            // 剩余部分还能构成一个"至少 16 字节用户区"的完整块才分割
            block_t* rest = (block_t*)((char*)node + sizeof(block_t) + need_size);
            init_free(rest, node->head.size - need_size - sizeof(block_t));
            free_list_->insert(node_of(rest));
            node->head.size = need_size;  // 取用部分缩小为正好需要的大小
            SPDLOG_LOGGER_TRACE(get_logger(), "malloc: split free block, remainder {} bytes -> free list",
                                rest->head.size);
        }
        // 从空闲链表摘除 -> 标记已分配 (节点不再需要, 用户区交给数据)
        free_list_->remove(free_node);
        node->head.inuse = true;
#ifdef DEBUG
        track_alloc(node);
#endif
        SPDLOG_LOGGER_DEBUG(get_logger(), "malloc: reuse free block, start: {:p} (user {} bytes)", (void*)(node + 1),
                            node->head.size);
        pthread_mutex_unlock(&locker_);
        return (void*)(node + 1);  // 跳过 block_t, 返回用户区
    }
    // 空闲链表没有合适块: 通过内存提供者申请新内存
    void* now_addr = memory_->allocate(total_size);
    // failed
    if (!now_addr)
    {
        SPDLOG_LOGGER_ERROR(get_logger(), "malloc failed, errno: {} ({})", errno, strerror(errno));
        pthread_mutex_unlock(&locker_);
        return nullptr;
    }
    // success: 新块标记为已分配
    block_t* node = (block_t*)now_addr;
    init_allocated(node, total_size - sizeof(block_t));
#ifdef DEBUG
    track_alloc(node);
#endif
    pthread_mutex_unlock(&locker_);
    SPDLOG_LOGGER_DEBUG(get_logger(), "malloc: extend heap via memory provider, total {} bytes (user {}), start: {:p}",
                        total_size, size, (void*)(node + 1));
    // remove block header: 用户只看到 block_t 之后的区域
    return (void*)(node + 1);
}

// ----------------------------------------------------------------------------
// Allocator::free
// ----------------------------------------------------------------------------
// 流程:
//   1. 由用户指针回退得到块头, 校验合法性 (owns_address + inuse 标志,
//      防 double free / 非法指针);
//   2. 合并物理相邻的空闲块 (通过空闲链表查找, 不假设堆形状/边界);
//   3. 释放委托给内存提供者 (IMemory):
//        - 支持随机释放 → 直接归还;
//        - 否则 (如 sbrk 栈式) → 按"申请顺序反向释放": release_block 仅对
//          最后申请的块成功, 成功后连带其下方紧邻的空闲块继续反向归还;
//          失败则挂回空闲链表复用。
void Allocator::free(void* addr)
{
    if (!addr)
    {
        return;  // free(NULL) 是合法空操作
    }
    block_t* node = (block_t*)addr - 1;  // 回退 16 字节拿到块头
    pthread_mutex_lock(&locker_);
    // 合法性校验: 地址必须在提供者空间内, 且 inuse 必须为 true
    // (inuse=false 或地址已归还/越界 = double free 或非法指针)
    if (!memory_->owns_address(node) || !node->head.inuse)
    {
        SPDLOG_LOGGER_ERROR(get_logger(), "free: double free or no malloc, start: {:p}", (void*)node);
        pthread_mutex_unlock(&locker_);
        return;
    }
    SPDLOG_LOGGER_DEBUG(get_logger(), "free: addr {:p} -> block {:p} (user {} bytes)", addr, (void*)node,
                        node->head.size);
#ifdef DEBUG
    track_free(addr);
#endif

    // --- 合并 (coalescing): 与空闲链表中的物理相邻块合并 ---
    // 通过链表查找物理相邻者, 不直接做地址运算 —— 不假设地址连续/边界,
    // 对非栈式内存提供者也安全。
    // 1) 后继合并: node 吞并后继 succ (keep=node, absorbed=succ)
    ListNode* succ_node = free_list_->find_next_phys(node_of(node));
    if (succ_node)
    {
        block_t* succ = block_of(succ_node);
        free_list_->remove(succ_node);
        node->head.size += sizeof(block_t) + succ->head.size;
        SPDLOG_LOGGER_TRACE(get_logger(), "free: coalesce next block {:p}, block now {} bytes", (void*)succ,
                            node->head.size);
    }
    // 2) 前驱合并: node 并入前驱 pred (keep=pred, absorbed=node)
    ListNode* pred_node = free_list_->find_prev_phys(node_of(node));
    if (pred_node)
    {
        block_t* pred = block_of(pred_node);
        free_list_->remove(pred_node);
        pred->head.size += sizeof(block_t) + node->head.size;
        node = pred;  // 合并后的块用前驱表示
        SPDLOG_LOGGER_TRACE(get_logger(), "free: coalesce into prev block {:p}, block now {} bytes", (void*)node,
                            node->head.size);
    }

    // --- 释放: 委托给内存提供者 (能力由提供者决定) ---
    if (memory_->supports_random_release())
    {
        // 支持随机释放: 任何块都能独立归还, 直接释放
        SPDLOG_LOGGER_TRACE(get_logger(), "free: release block {:p} ({} bytes) to memory provider", (void*)node,
                            node->head.size);
        memory_->release_block(node, sizeof(block_t) + node->head.size);
    }
    else
    {
        // 不支持随机释放 (如 sbrk 栈式): 只能"按申请顺序反向释放" —
        // release_block 仅对"最后申请的块"成功; 成功后连带其下方紧邻的
        // 空闲块继续反向归还 (它们在堆中现在也贴住边界了)。
        block_t* p = node;
        for (;;)
        {
            if (memory_->release_block(p, sizeof(block_t) + p->head.size))
            {
                // 归还成功: 尝试继续反向归还其下方紧邻的空闲块
                ListNode* prev_node = free_list_->find_prev_phys(node_of(p));
                if (!prev_node)
                {
                    break;  // 没有更多可连带归还的
                }
                block_t* prev = block_of(prev_node);
                free_list_->remove(prev_node);
                p = prev;
            }
            else
            {
                // 无法归还 (不是最后申请的块): 挂回空闲链表复用
                SPDLOG_LOGGER_TRACE(get_logger(), "free: block {:p} (user {} bytes) -> free list", (void*)(p + 1),
                                    p->head.size);
                init_free(p, p->head.size);
                free_list_->insert(node_of(p));
                break;
            }
        }
    }
    pthread_mutex_unlock(&locker_);
}

// ----------------------------------------------------------------------------
// Allocator::calloc = malloc + 清零
// ----------------------------------------------------------------------------
void* Allocator::calloc(size_t num, size_t size)
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
    void* addr = malloc(total_size);
    if (!addr)
    {
        return nullptr;
    }
    memset(addr, 0, total_size);  // 置零
    SPDLOG_LOGGER_TRACE(get_logger(), "calloc: {} x {} = {} bytes, zeroed", num, size, total_size);
    return addr;
}

// ----------------------------------------------------------------------------
// Allocator::realloc
// ----------------------------------------------------------------------------
// 语义 (与标准 realloc 对齐):
//   - realloc(ptr, 0)   -> 释放 ptr 并返回 NULL;
//   - realloc(NULL, n)  -> 等价 malloc(n);
//   - 原块够大          -> 原地返回, 不动数据;
//   - 原块不够大        -> 新分配 + 拷贝旧数据 + 释放旧块。
void* Allocator::realloc(void* addr, size_t size)
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
    block_t* node = (block_t*)addr - 1;
    if (node->head.size >= size)
    {
        SPDLOG_LOGGER_TRACE(get_logger(), "realloc: in-place, old {} >= new {}", node->head.size, size);
        return addr;  // 原地满足, 直接返回 (不分割, 教学取舍)
    }
    // 需要扩容: 重新分配, 拷贝旧数据, 释放旧块
    SPDLOG_LOGGER_DEBUG(get_logger(), "realloc: growing {:p} from {} to {} bytes", addr, node->head.size, size);
    void* new_addr = malloc(size);
    if (new_addr)
    {
        // 只拷贝旧块实际可用的大小, 避免越界读
        memcpy(new_addr, addr, node->head.size);
        free(addr);
        SPDLOG_LOGGER_TRACE(get_logger(), "realloc: moved to {:p} (copied {} bytes)", new_addr, node->head.size);
    }
    return new_addr;
}

#ifdef DEBUG
// ----------------------------------------------------------------------------
// DEBUG 统计与泄漏检测
// ----------------------------------------------------------------------------
// 每次 malloc 登记 (用户指针 + 大小), 每次 free 注销; 程序退出时析构函数
// 打印仍未释放的块 —— 泄漏检测。用 spdlog: get_logger() 返回进程生命周期的
// logger (见 log.cpp), 全局对象析构阶段调用依然安全。
void Allocator::track_alloc(block_t* node)
{
    if (tracked_count_ < kMaxTracked)
    {
        tracked_[tracked_count_] = (void*)(node + 1);
        tracked_size_[tracked_count_] = node->head.size;
        ++tracked_count_;
    }
}

void Allocator::track_free(void* user_ptr)
{
    for (size_t i = 0; i < tracked_count_; ++i)
    {
        if (tracked_[i] == user_ptr)
        {
            tracked_[i] = tracked_[tracked_count_ - 1];
            tracked_size_[i] = tracked_size_[tracked_count_ - 1];
            --tracked_count_;
            return;
        }
    }
    get_logger()->warn("DEBUG: free of untracked pointer {:p} (double free or bug?)", user_ptr);
}

Allocator::~Allocator()
{
    if (tracked_count_ > 0)
    {
        size_t total = 0;
        for (size_t i = 0; i < tracked_count_; ++i)
        {
            total += tracked_size_[i];
        }
        get_logger()->error("MEMORY LEAK DETECTED: {} block(s) not freed ({} bytes total):", tracked_count_, total);
        for (size_t i = 0; i < tracked_count_; ++i)
        {
            get_logger()->error("  leak: {:p} ({} bytes)", tracked_[i], tracked_size_[i]);
        }
    }
    else
    {
        get_logger()->info("no memory leaks (all allocations freed)");
    }
}

void Allocator::dump_stats()
{
    // 锁外读取统计/链表状态, 必须加锁与分配/释放互斥
    pthread_mutex_lock(&locker_);
    get_logger()->info("stats: allocated_blocks={} allocated_bytes={} free_blocks={}", tracked_count_,
                       total_alloc_bytes_, free_list_->size());
    pthread_mutex_unlock(&locker_);
}
#endif  // DEBUG
}  // namespace wageco
