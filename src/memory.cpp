// ============================================================================
// memory.cpp - 基于 sbrk 的内存提供者 (IMemory 实现)
// ----------------------------------------------------------------------------
// SbrkMemory 实现 IMemory 的能力契约, 释放采用"物理判据":
//   - allocate:   sbrk(size) 上移断点, 返回旧断点 (新空间起始);
//   - supports_random_release: false —— sbrk 堆是 LIFO 形状, 只能
//                 按申请顺序反向释放;
//   - release_block(addr, size): 仅当 块末尾(addr+size) == 当前断点
//                 时才用 brk 归还 (该块是最后申请的 / 物理堆顶);
//                 否则返回 false (分配器挂回空闲链表复用);
//                 物理判据天然支持块合并: 合并块的末尾贴着断点即可归还;
//   - owns_address(addr): 地址在 [堆起点, 当前断点) 内。
// 换内存策略 (如 mmap, 支持随机释放) 只需提供新的 IMemory 实现并在组合根
// 注入, 分配器零改动。
// ============================================================================
#include <stdint.h>  // uintptr_t (指针比较转整数, 避免 UB)
#include <unistd.h>

#include "internal.h"

namespace wageco
{
// 记录"本提供者感知的堆起点": 构造时 (首次分配前) 的断点位置。
// 之后所有本库申请的块都位于 [heap_start_, ...) 内, 用于 owns_address 校验。
SbrkMemory::SbrkMemory() : heap_start_(sbrk(0)) {}

void* SbrkMemory::allocate(size_t size)
{
    void* p = sbrk(size);
    if (p == (void*)-1)
    {
        return nullptr;
    }
#ifdef DEBUG
    alloc_bytes_ += size;
#endif
    return p;
}

bool SbrkMemory::supports_random_release() const
{
    // sbrk 堆是 LIFO 形状: 只能从堆顶往回收缩, 不支持随机释放
    return false;
}

bool SbrkMemory::release_block(void* addr, size_t size)
{
    // 物理判据: 仅当该块"物理上贴住当前边界" (块末尾 == 当前断点,
    // 即最后申请的块, 申请顺序的反向) 才可归还; 合并块的末尾贴着断点
    // 时同样成立, 无需额外同步。用整数比较而非指针比较 (避免 UB)。
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
#ifdef DEBUG
    release_bytes_ += size;
#endif
    return true;
}

#ifdef DEBUG
// DEBUG: 析构时打印本提供者的申请/归还统计
SbrkMemory::~SbrkMemory()
{
    get_logger()->info("sbrk provider: allocated {} bytes, released {} bytes, outstanding {} bytes", alloc_bytes_,
                       release_bytes_, (ssize_t)(alloc_bytes_ - release_bytes_));
}
#endif  // DEBUG

bool SbrkMemory::owns_address(const void* addr) const
{
    // 地址必须在 [堆起点, 当前断点) 内; 用整数比较避免指针比较 UB
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    return a >= reinterpret_cast<uintptr_t>(heap_start_) && a < reinterpret_cast<uintptr_t>(sbrk(0));
}
}  // namespace wageco
