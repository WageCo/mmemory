// ============================================================================
// tcache.cpp - 线程本地缓存 (Tcache) 实现
// ----------------------------------------------------------------------------
// 类声明在 include/tcache.h。
// 设计要点:
//   - 档位 = 对齐后的用户区大小 (16B 一档, 16~1024B 共 64 档), 与
//     block_t::head.size 精确一致, 出缓存无需 split;
//   - 链表节点复用空闲块用户区前 8 字节 (块在缓存中, 用户区无数据),
//     零额外开销;
//   - pop/push 都是 O(1), 且无锁 (实例为 thread_local, 仅本线程访问);
//   - 档满时整档倒回全局分配器 (drain_ 回调): 块回到空闲链表, 参与
//     物理合并与反向释放 —— 缓存只是"延迟回收", 不阻止回收。
// ============================================================================
#include "internal.h"

namespace wageco
{
// ----------------------------------------------------------------------------
// 档位计算
// ----------------------------------------------------------------------------
// 请求 size -> 档位: 与 Allocator::malloc 相同的对齐公式 ——
//   总占用 = 16 对齐(block_t + size), 用户区 = 总占用 - block_t;
// 装配点保证只在本档覆盖范围内调用 (size <= kMaxBytes), 无溢出风险。
size_t Tcache::bin_of_request(size_t size)
{
    const size_t total = (sizeof(block_t) + size + align_to - 1) & ~(align_to - 1);
    const size_t user_size = total - sizeof(block_t);
    return user_size / align_to - 1;
}

// 块头用户区大小 -> 档位: head.size 恒为 16 的倍数 (分配器不变量),
// 16B -> 0, 32B -> 1, ..., 1024B -> 63。
size_t Tcache::bin_of_block(size_t user_size) { return user_size / align_to - 1; }

// ----------------------------------------------------------------------------
// 弹出 (缓存命中) / 压入
// ----------------------------------------------------------------------------
// 命中: 摘链, 计数 -1, inuse 置回 true (用户区重新交给数据), 返回用户指针。
void* Tcache::pop(size_t bin)
{
    block_t* node = bins_[bin];
    if (!node)
    {
        return nullptr;
    }
    bins_[bin] = entry_of(node)->next;
    --counts_[bin];
    node->head.inuse = true;
    return (void*)(node + 1);
}

// 压入: 插到档位链表头部, 计数 +1; 档满返回 false (由调用方决定倒回)。
// node 的用户区前 8 字节写入 next, 覆盖旧数据 —— 块已空闲, 无数据可丢。
bool Tcache::push(block_t* node)
{
    const size_t bin = bin_of_block(node->head.size);
    if (counts_[bin] >= kBinLimit)
    {
        return false;
    }
    entry_of(node)->next = bins_[bin];
    bins_[bin] = node;
    ++counts_[bin];
    return true;
}

size_t Tcache::count() const
{
    size_t total = 0;
    for (size_t i = 0; i < kBinCount; ++i)
    {
        total += counts_[i];
    }
    return total;
}

// ----------------------------------------------------------------------------
// 倒回全局分配器
// ----------------------------------------------------------------------------
// 把某一档所有缓存块依次交给 drain_ 回调 (装配点 = 全局分配器的 free):
//   - 归还前恢复 inuse=true —— 分配器的 free 用 inuse 校验合法性
//     (块进缓存时被置为 false, 直接交还会被当成 double free 拒绝);
//   - 之后块在分配器内走原有流程: 物理合并 -> 反向释放或挂回空闲链表,
//     缓存状态与分配器状态重新一致。
void Tcache::flush_bin(size_t bin)
{
    while (bins_[bin])
    {
        block_t* node = bins_[bin];
        bins_[bin] = entry_of(node)->next;
        --counts_[bin];
        node->head.inuse = true;  // 交还前恢复"已分配"标志
        drain_(node);
    }
}

void Tcache::flush_all()
{
    for (size_t i = 0; i < kBinCount; ++i)
    {
        flush_bin(i);
    }
}
}  // namespace wageco
