// ============================================================================
// memory.h - 内存提供者层: IMemory / SbrkMemory
// ----------------------------------------------------------------------------
// IMemory 定义"申请 + 释放能力"契约, 分配器的释放逻辑完全委托给提供者,
// 不假设任何"堆形状":
//   - supports_random_release(): 是否支持"随机释放" (任意块独立归还);
//   - release_block(addr, size): 归还一块内存 ——
//       随机释放提供者 (如 mmap): 任何块都能归还 (返回 true);
//       非随机提供者 (如 sbrk 栈式): 仅当块末尾(addr+size) 贴住当前边界
//       (最后申请的块, 申请顺序的反向) 才归还, 否则返回 false;
//   - owns_address(): 地址是否在本提供者的空间内 (free 合法性粗校验)。
// ============================================================================
#ifndef MMEMORY_MEMORY_H
#define MMEMORY_MEMORY_H

#include <stddef.h>  // size_t

namespace wageco
{
// ----------------------------------------------------------------------------
// IMemory - 内存提供者抽象 (申请 + 释放能力)
// ----------------------------------------------------------------------------
class IMemory
{
   public:
    virtual ~IMemory() = default;

    // 向系统申请 size 字节, 返回连续空间起始地址; 失败返回 nullptr
    virtual void* allocate(size_t size) = 0;

    // 是否支持"随机释放": 任意块都可独立归还给系统
    virtual bool supports_random_release() const = 0;

    // 归还"申请时返回的地址"所对应的块:
    //   - addr 必须是 allocate() 返回的地址 (块起始), 不能是其他地址;
    //   - size 是该块总大小 (block 头 + 用户区), 提供者用 addr+size 判断
    //     块末尾是否贴住当前边界 (即该块是否为最后申请的 / 物理堆顶);
    //   - 随机释放提供者: 总是成功 (返回 true);
    //   - 非随机 (栈式) 提供者: 仅当该块物理上贴住边界 (申请顺序的反向)
    //     才归还, 否则返回 false
    virtual bool release_block(void* addr, size_t size) = 0;

    // addr 是否在本提供者的空间内 (用于 free 的合法性粗校验)
    virtual bool owns_address(const void* addr) const = 0;
};

// ----------------------------------------------------------------------------
// SbrkMemory - 基于 sbrk() 的内存提供者 (定义见 src/memory.cpp)
// ----------------------------------------------------------------------------
// 释放采用"物理判据": release_block 仅当 块末尾(addr+size) == 当前断点
// 时才用 brk 归还 —— 这等价于"按申请顺序反向释放"(堆天然是栈),
// 且天然支持块合并 (合并块末尾贴着断点即可归还, 无需额外同步)。
class SbrkMemory : public IMemory
{
   public:
    SbrkMemory();  // 记录堆起点 (首次分配前)

    void* allocate(size_t size) override;
    bool supports_random_release() const override;
    bool release_block(void* addr, size_t size) override;
    bool owns_address(const void* addr) const override;

#ifdef DEBUG
    ~SbrkMemory();  // DEBUG: 析构时打印申请/归还字节统计
#endif

   private:
    void* heap_start_;  // 本提供者感知的堆起点 (首次分配前记录)

#ifdef DEBUG
    size_t alloc_bytes_ = 0;    // 累计申请字节 (DEBUG)
    size_t release_bytes_ = 0;  // 累计归还字节 (DEBUG)
#endif
};

}  // namespace wageco
#endif  // MMEMORY_MEMORY_H
