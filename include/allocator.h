// ============================================================================
// allocator.h - 分配器层: Allocator 声明
// ----------------------------------------------------------------------------
// Allocator 由"存储模式 + 内存申请 + 查找策略"三个注入依赖组合而成:
//   1) 存储模式: free_list (IList 实现, 管理空闲块);
//   2) 内存申请: memory (IMemory 实现, 任意来源);
//   - 分配 (malloc): 空闲链表 first-fit 复用, 否则 memory_->allocate 申请;
//   - 释放 (free): 校验 (owns_address + inuse) → 合并物理相邻空闲块 →
//     委托 memory_->release_block: 支持随机释放则直接归还; 否则按"申请
//     顺序反向释放" (release_block 成功时连带下方紧邻空闲块继续反向归还,
//     失败则挂回空闲链表复用) —— 分配器不含任何栈式回收代码;
//   - 不再维护"已分配链表": 块是否已分配由 block_t 的 inuse 标志标识;
//   - 持有互斥锁 locker_, 保证链表操作与内存申请的组合操作原子性。
// 实现位置: src/allocator.cpp; 全局实例与公共 API 转发见 src/mmemory.cpp。
// 扩展点: 换存储模式 (分 bin 表/树) 或内存策略 (mmap) 都只改组合根。
// ============================================================================
#ifndef MMEMORY_ALLOCATOR_H
#define MMEMORY_ALLOCATOR_H

#include <pthread.h>  // pthread_mutex_t
#include <stddef.h>   // size_t

#include "find_strategy.h"  // IFindStrategy
#include "list.h"           // IList
#include "memory.h"         // IMemory

namespace wageco
{
class Allocator
{
   public:
    // 依赖注入: "空闲链表"存储模式 + "内存提供者" + "查找策略"
    Allocator(IList* free_list, IMemory* memory, IFindStrategy* strategy)
        : free_list_(free_list), memory_(memory), strategy_(strategy)
    {
    }

    // 分配 size 字节, 返回 16 字节对齐的用户指针; 失败返回 nullptr
    void* malloc(size_t size);
    // 释放 malloc/calloc/realloc 返回的指针; nullptr 是合法参数
    void free(void* addr);
    // 分配 num*size 字节并清零; 溢出时返回 nullptr
    void* calloc(size_t num, size_t size);
    // 调整大小; 语义与标准 realloc 一致
    void* realloc(void* addr, size_t size);

#ifdef DEBUG
    // 泄漏检测析构: 程序退出时打印未释放的块 (仅 DEBUG 构建)
    ~Allocator();
    // 打印当前分配统计: 已分配块数 / 总字节 / 空闲块数 (仅 DEBUG 构建)
    void dump_stats();
#endif

   private:
    IList* free_list_;                                    // "空闲块" 存储模式 (注入)
    IMemory* memory_;                                     // 内存申请来源 (注入, 任意提供者)
    IFindStrategy* strategy_;                             // 空闲块查找策略 (注入)
    pthread_mutex_t locker_ = PTHREAD_MUTEX_INITIALIZER;  // 并发保护 (全局锁)

#ifdef DEBUG
    // --- DEBUG: 分配统计与泄漏登记 ---
    static constexpr size_t kMaxTracked = 1 << 16;  // 泄漏登记容量 (教学演示)
    void* tracked_[kMaxTracked];                    // 已分配用户指针
    size_t tracked_size_[kMaxTracked];              // 对应块大小
    size_t tracked_count_ = 0;                      // 当前未释放块数
    size_t total_alloc_bytes_ = 0;                  // 当前已分配用户字节总数

    void track_alloc(block_t* node);  // 登记一次分配
    void track_free(void* user_ptr);  // 注销一次释放
#endif
};

}  // namespace wageco
#endif  // MMEMORY_ALLOCATOR_H
