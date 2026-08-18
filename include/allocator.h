// ============================================================================
// allocator.h - 分配器层: Allocator (模板版, 编译期多态)
// ----------------------------------------------------------------------------
// 本分支 (template_c++11) 的模板化要点 (对比 master 的虚函数注入版):
//   - IList/IMemory/IFindStrategy 三个虚接口被删除, 依赖改为模板参数:
//       Allocator<ListT, MemoryT, StrategyT>
//     三个依赖在编译期绑定具体类型, 零虚函数调用开销;
//   - 模板参数约束 (编译期鸭子类型):
//       ListT     需提供 insert/remove/find_first_fit/contains/
//                 find_prev_phys/find_next_phys/for_each 等非虚方法;
//       MemoryT   需提供 allocate/supports_random_release/release_block/
//                 owns_address 等非虚方法;
//       StrategyT 需提供 find(ListT&, size_t) 模板成员 (见 find_strategy.h);
//   - 实现全部在头文件 (模板必须在实例化点可见), 失去声明/实现分离
//     —— 这是编译期多态的固有代价;
//   - 职责与算法逻辑与 master 完全一致 (见下方注释), 仅"如何注入依赖"改变。
//
// 职责 (与 master 相同):
//   - 只维护"空闲链表" (free_list_, 注入的 ListT);
//   - 块是否已分配由 block_t 的 inuse 标志标识 (无"已分配链表");
//   - 分配 (malloc): 空闲链表按策略查找复用, 否则 memory_->allocate 申请;
//   - 释放 (free): 校验 (owns_address + inuse) → 合并物理相邻空闲块 →
//     委托 memory_->release_block: 支持随机释放则直接归还; 否则按"申请
//     顺序反向释放" (release_block 成功时连带下方紧邻空闲块继续反向归还,
//     失败则挂回空闲链表复用);
//   - 持有互斥锁 locker_, 保证链表操作与内存申请的组合操作原子性。
// 全局实例与公共 API 转发见 src/mmemory.cpp。
// 扩展点: 换存储模式/内存策略/查找策略都只改组合根的模板实参。
// ============================================================================
#ifndef MMEMORY_ALLOCATOR_H
#define MMEMORY_ALLOCATOR_H

#include <errno.h>
#include <pthread.h>  // pthread_mutex_t
#include <stddef.h>   // size_t
#include <stdlib.h>
#include <string.h>

#include "find_strategy.h"  // 策略模板 (FirstFit/BestFit)
#include "list.h"           // HeaderList / ListNode / init_free
#include "logging.h"        // get_logger / SPDLOG_LOGGER_* 宏
#include "memory.h"         // SbrkMemory

namespace wageco
{
template <typename ListT, typename MemoryT, typename StrategyT>
class Allocator
{
   public:
    // 依赖注入 (编译期): "空闲链表"存储模式 + "内存提供者" + "查找策略"
    Allocator(ListT* free_list, MemoryT* memory, StrategyT* strategy)
        : free_list_(free_list), memory_(memory), strategy_(strategy)
    {
    }

    // 分配 size 字节, 返回 16 字节对齐的用户指针; 失败返回 nullptr
    void* malloc(size_t size)
    {
        if (!size)
        {
            return nullptr;  // 语义: malloc(0) 允许返回 NULL
        }
        // 防护: 总占用计算 (block + size + 对齐) 不得溢出 size_t, 否则回绕
        // 会算出极小值导致错误分配
        if (size > (size_t)-1 - sizeof(block_t) - align_to)
        {
            get_logger()->error("malloc: request too large ({} bytes)", size);
            return nullptr;
        }
        pthread_mutex_lock(&locker_);
        // 总占用 = block_t(16) + 用户 size, 向上对齐到 16 的倍数
        // 例如: size=1   -> total = (16+1+15)&~15 = 32
        //        size=500 -> total = (16+500+15)&~15 = 528
        size_t total_size = (sizeof(block_t) + size + align_to - 1) & ~(align_to - 1);
        SPDLOG_LOGGER_DEBUG(get_logger(), "malloc: request {} bytes (total {} with block header)", size, total_size);
        // search free list: 按注入的查找策略找可用块 (链表返回节点, 转回宿主块)
        ListNode* free_node = strategy_->find(*free_list_, total_size - sizeof(block_t));
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
            SPDLOG_LOGGER_DEBUG(get_logger(), "malloc: reuse free block, start: {:p} (user {} bytes)",
                                (void*)(node + 1), node->head.size);
            pthread_mutex_unlock(&locker_);
            return (void*)(node + 1);  // 跳过 block_t, 返回用户区
        }
        // 空闲链表没有合适块: 通过内存提供者申请新内存
        void* now_addr = memory_->allocate(total_size);
        // failed
        if (!now_addr)
        {
            get_logger()->error("malloc failed, errno: {} ({})", errno, strerror(errno));
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
        SPDLOG_LOGGER_DEBUG(get_logger(),
                            "malloc: extend heap via memory provider, total {} bytes (user {}), start: {:p}",
                            total_size, size, (void*)(node + 1));
        // remove block header: 用户只看到 block_t 之后的区域
        return (void*)(node + 1);
    }

    // 释放 malloc/calloc/realloc 返回的指针; nullptr 是合法参数
    void free(void* addr)
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
            get_logger()->error("free: double free or no malloc, start: {:p}", (void*)node);
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

    // 分配 num*size 字节并清零; 溢出时返回 nullptr
    void* calloc(size_t num, size_t size)
    {
        if (!num || !size)
        {
            return nullptr;
        }
        // 溢出检查: num * size 可能超过 size_t 上限
        if (num > (size_t)-1 / size)
        {
            get_logger()->error("calloc: size overflow ({} x {})", num, size);
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

    // 调整大小; 语义与标准 realloc 一致:
    //   - realloc(ptr, 0)   -> 释放 ptr 并返回 NULL;
    //   - realloc(NULL, n)  -> 等价 malloc(n);
    //   - 原块够大          -> 原地返回, 不动数据;
    //   - 原块不够大        -> 新分配 + 拷贝旧数据 + 释放旧块。
    void* realloc(void* addr, size_t size)
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
    // 泄漏检测析构: 程序退出时打印未释放的块 (仅 DEBUG 构建)
    ~Allocator()
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

    // 打印当前分配统计: 已分配块数 / 总字节 / 空闲块数 (仅 DEBUG 构建)
    void dump_stats()
    {
        // 锁外读取统计/链表状态, 必须加锁与分配/释放互斥
        pthread_mutex_lock(&locker_);
        get_logger()->info("stats: allocated_blocks={} allocated_bytes={} free_blocks={}", tracked_count_,
                           total_alloc_bytes_, free_list_->size());
        pthread_mutex_unlock(&locker_);
    }
#endif

   private:
    ListT* free_list_;                                    // "空闲块" 存储模式 (模板注入)
    MemoryT* memory_;                                     // 内存申请来源 (模板注入)
    StrategyT* strategy_;                                 // 空闲块查找策略 (模板注入)
    pthread_mutex_t locker_ = PTHREAD_MUTEX_INITIALIZER;  // 并发保护 (全局锁)

#ifdef DEBUG
    // --- DEBUG: 分配统计与泄漏登记 ---
    static constexpr size_t kMaxTracked = 1 << 16;  // 泄漏登记容量 (教学演示)
    void* tracked_[kMaxTracked];                    // 已分配用户指针
    size_t tracked_size_[kMaxTracked];              // 对应块大小
    size_t tracked_count_ = 0;                      // 当前未释放块数
    size_t total_alloc_bytes_ = 0;                  // 当前已分配用户字节总数

    // 每次 malloc 登记 (用户指针 + 大小), 每次 free 注销; 程序退出时析构函数
    // 打印仍未释放的块 —— 泄漏检测。
    void track_alloc(block_t* node)
    {
        if (tracked_count_ < kMaxTracked)
        {
            tracked_[tracked_count_] = (void*)(node + 1);
            tracked_size_[tracked_count_] = node->head.size;
            ++tracked_count_;
        }
    }

    void track_free(void* user_ptr)
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
#endif
};

}  // namespace wageco
#endif  // MMEMORY_ALLOCATOR_H
