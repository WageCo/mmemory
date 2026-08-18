// ============================================================================
// memory.h - 内存提供者层: SbrkMemory (模板分支版, 无虚接口, header-only)
// ----------------------------------------------------------------------------
// 本分支 (template_c++11) 的模板化要点 (对比 master):
//   - IMemory 虚接口被删除: 提供者能力契约改由"编译期 trait + 模板参数约束"
//     表达 —— 关键能力 (是否支持随机释放) 用 memory_traits 编译期声明,
//     Allocator 据此编译期分派 (tag dispatch), 未选分支不实例化;
//   - SbrkMemory 从"实现 IMemory"变为普通类 (方法全部内联在头文件,
//     与分支的 header-only 风格一致), 语义不变:
//       - release_block(addr, size): 归还一块内存 ——
//           非随机提供者 (如 sbrk 栈式): 仅当块末尾(addr+size) 贴住当前边界
//           (最后申请的块, 申请顺序的反向) 才归还, 否则返回 false;
//       - owns_address(): 地址是否在本提供者的空间内 (free 合法性粗校验);
//       - supports_random_release() 已删除 —— 由 memory_traits 编译期取代;
//   - 断点缓存 (与 master 一致): 独占堆契约下本库是唯一 sbrk 使用者,
//     缓存"当前断点" cur_break_ (allocate/release_block 时同步更新),
//     owns_address 只做内存比较, 不在热路径调用 sbrk(0)。
// ============================================================================
#ifndef MMEMORY_MEMORY_H
#define MMEMORY_MEMORY_H

#include <stddef.h>  // size_t
#include <stdint.h>  // uintptr_t (指针比较转整数, 避免 UB)
#include <unistd.h>  // sbrk / brk

#include "logging.h"  // get_logger (DEBUG 析构统计)

namespace wageco
{
// ----------------------------------------------------------------------------
// memory_traits - 内存提供者编译期能力 trait
// ----------------------------------------------------------------------------
// "是否支持随机释放"是类型的固有属性 (sbrk 恒 false, mmap 恒 true),
// 用编译期 trait 表达 —— Allocator 据此在编译期分派释放路径
// (tag dispatch), 未选中的分支不会被实例化。这是模板独有能力,
// 取代了运行时虚查询 (master 的 IMemory::supports_random_release())。
template <typename MemoryT>
struct memory_traits
{
    // 默认: 非随机释放 (sbrk 栈式堆); 支持随机释放的提供者特化为 true
    static constexpr bool random_release = false;
};

// 未来 mmap 提供者示例:
//   template <> struct memory_traits<MmapMemory> { static constexpr bool random_release = true; };

// ----------------------------------------------------------------------------
// SbrkMemory - 基于 sbrk() 的内存提供者 (普通类, 方法全部内联)
// ----------------------------------------------------------------------------
// 释放采用"物理判据": release_block 仅当 块末尾(addr+size) == 当前断点
// 时才用 brk 归还 —— 这等价于"按申请顺序反向释放"(堆天然是栈),
// 且天然支持块合并 (合并块末尾贴着断点即可归还, 无需额外同步)。
class SbrkMemory
{
   public:
    // 记录"本提供者感知的堆起点与当前断点": 构造时 (首次分配前) 的断点位置。
    // 之后所有本库申请的块都位于 [heap_start_, cur_break_) 内, 用于
    // owns_address 校验。独占堆契约下本库是唯一 sbrk 使用者, cur_break_
    // 由 allocate/release_block 同步维护, owns_address 不必调 sbrk(0)。
    SbrkMemory() : heap_start_(sbrk(0)), cur_break_(sbrk(0)) {}

    // 向系统申请 size 字节, 返回连续空间起始地址; 失败返回 nullptr
    void* allocate(size_t size)
    {
        void* p = sbrk(size);
        if (p == (void*)-1)
        {
            return nullptr;
        }
        cur_break_ = (char*)p + size;  // 同步缓存断点
#ifdef DEBUG
        alloc_bytes_ += size;
#endif
        return p;
    }

    // 归还"申请时返回的地址"所对应的块:
    //   - addr 必须是 allocate() 返回的地址 (块起始), 不能是其他地址;
    //   - size 是该块总大小 (block 头 + 用户区);
    //   - 物理判据: 仅当块末尾 == 当前断点 (最后申请的块 / 物理堆顶) 才
    //     用 brk 归还, 否则返回 false (分配器挂回空闲链表复用);
    //   - 合并块的末尾贴着断点同样成立, 无需额外同步。
    bool release_block(void* addr, size_t size)
    {
        // 用整数比较而非指针比较 (避免 UB)
        const uintptr_t block_end = reinterpret_cast<uintptr_t>(addr) + size;
        if (block_end != reinterpret_cast<uintptr_t>(sbrk(0)))
        {
            return false;
        }
        // 把断点直接设置回该块起始 (brk 失败返回 -1, 成功返回 0)
        if (brk(addr) != 0)
        {
            return false;
        }
        cur_break_ = addr;  // 同步缓存断点
#ifdef DEBUG
        release_bytes_ += size;
#endif
        return true;
    }

    // addr 是否在本提供者的空间内 (用于 free 的合法性粗校验):
    // 地址必须在 [堆起点, 当前断点) 内; 断点取缓存值 (独占堆契约)。
    bool owns_address(const void* addr) const
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
        return a >= reinterpret_cast<uintptr_t>(heap_start_) && a < reinterpret_cast<uintptr_t>(cur_break_);
    }

#ifdef DEBUG
    // DEBUG: 析构时打印本提供者的申请/归还统计
    ~SbrkMemory()
    {
        get_logger()->info("sbrk provider: allocated {} bytes, released {} bytes, outstanding {} bytes", alloc_bytes_,
                           release_bytes_, (ssize_t)(alloc_bytes_ - release_bytes_));
    }
#endif

   private:
    void* heap_start_;  // 本提供者感知的堆起点 (首次分配前记录)
    void* cur_break_;   // 缓存当前断点 (独占堆契约下由本库维护)

#ifdef DEBUG
    size_t alloc_bytes_ = 0;    // 累计申请字节 (DEBUG)
    size_t release_bytes_ = 0;  // 累计归还字节 (DEBUG)
#endif
};

}  // namespace wageco
#endif  // MMEMORY_MEMORY_H
