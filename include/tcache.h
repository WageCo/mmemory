// ============================================================================
// tcache.h - 线程本地缓存层 (Tcache, 模板分支版)
// ----------------------------------------------------------------------------
// 本分支 (template_c++11) 的模板化要点 (对比 master):
//   - 缓存上限与每档块数从"类内 static constexpr"提升为模板参数:
//       Tcache<kMaxBytes, kBinLimit>
//     装配点可编译期定制 (如 Tcache<2048, 4>), 无需改类定义;
//   - 其余语义与 master 完全一致 (见下方注释)。
//
// 在"公共 API → 全局分配器"之间插入一层 per-thread 小对象缓存 (tcache):
//   - 只缓存"小对象": 对齐后的用户区大小 (block_t::head.size) <= kMaxBytes
//     (默认 1024B), 16 字节一档共 64 档 (16B ~ 1024B);
//   - 命中缓存: 零锁、零系统调用、零空闲链表查找 —— malloc/free 都是 O(1);
//   - 档位按"对齐后的用户区大小"精确匹配 (与 block_t::head.size 一致),
//     因此出缓存时必然正好满足请求, 无需 split;
//   - 每档最多 kBinLimit 块 (默认 7, glibc 的 tcache_count 默认值),
//     满了把整档倒回全局分配器 (倒回后参与合并与反向释放, 缓存只延迟回收);
//   - 线程私有: 实例为 thread_local, 只被本线程访问, 快路径完全无锁;
//   - 块在缓存中 inuse=false (空闲); 倒回全局分配器前恢复 inuse=true
//     (分配器的 free 用 inuse 标志校验)。
// 装配 (thread_local 实例 + 倒回回调) 见 src/mmemory.cpp。
// 说明: 本层只服务于"公共 API" (wageco::malloc/free), Allocator 本身
//       保持"存储模式 + 内存申请 + 查找策略"三依赖纯净, 不受影响。
// ============================================================================
#ifndef MMEMORY_TCACHE_H
#define MMEMORY_TCACHE_H

#include <stddef.h>  // size_t

#include "block.h"  // block_t / align_to

namespace wageco
{
template <size_t kMaxBytes = 1024, size_t kBinLimit = 7>
class Tcache
{
   public:
    // 编译期约束: 模板参数非法时给出可读诊断 (而不是算出一个坏数组)
    static_assert(kMaxBytes % align_to == 0, "kMaxBytes 必须是 16 的倍数 (对齐粒度)");
    static_assert(kMaxBytes >= align_to, "kMaxBytes 至少为 16");
    static_assert(kBinLimit > 0, "kBinLimit 必须大于 0");

    // 档数 = 上限 / 对齐粒度 (16B 一档): 1024/16 = 64 档 (16B ~ 1024B)
    static constexpr size_t kBinCount = kMaxBytes / align_to;

    // 倒回回调: 把一块缓存块交还给全局分配器 (由装配点注入, 见 src/mmemory.cpp)
    using DrainFn = void (*)(block_t* node);

    explicit Tcache(DrainFn drain) : drain_(drain) {}

    // 请求大小 size 是否走缓存 (size <= kMaxBytes 时, 对齐后的用户区大小
    // 必落在 16~kMaxBytes 档内, 且对齐运算不可能溢出)
    static bool covers(size_t size) { return size <= kMaxBytes; }
    // 请求 size -> 档位索引 (前提: covers(size); 对齐后用户区 16B->0, 32B->1, ...)
    static size_t bin_of_request(size_t size)
    {
        // 与 Allocator::malloc 相同的对齐公式: 总占用 = 16 对齐(16 + size),
        // 用户区 = 总占用 - 16。size <= kMaxBytes 时无溢出风险 (装配点已保证)。
        const size_t total = (sizeof(block_t) + size + align_to - 1) & ~(align_to - 1);
        const size_t user_size = total - sizeof(block_t);
        return user_size / align_to - 1;
    }
    // 块头用户区大小 -> 档位索引 (head.size 恒为 16 倍数, 16B->0, 32B->1, ...)
    static size_t bin_of_block(size_t user_size) { return user_size / align_to - 1; }

    // 从档位弹出一块 (缓存命中), 无则返回 nullptr; 命中后 inuse 置回 true
    void* pop(size_t bin)
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

    // 压入一块 (调用方需已把 inuse 置为 false); 档满返回 false
    bool push(block_t* node)
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

    // 档位是否已满
    bool full(size_t bin) const { return counts_[bin] >= kBinLimit; }

    // 当前缓存块总数 (DEBUG/统计用)
    size_t count() const
    {
        size_t total = 0;
        for (size_t i = 0; i < kBinCount; ++i)
        {
            total += counts_[i];
        }
        return total;
    }

    // 把某一档全部缓存块倒回全局分配器 (drain_ 回调), 清空该档:
    //   - 归还前恢复 inuse=true —— 分配器的 free 用 inuse 校验合法性
    //     (块进缓存时被置为 false, 直接交还会被当成 double free 拒绝);
    //   - 之后块在分配器内走原有流程: 物理合并 -> 反向释放或挂回空闲链表。
    void flush_bin(size_t bin)
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

    // 把全部档倒回全局分配器 (程序退出/显式调用/测试)
    void flush_all()
    {
        for (size_t i = 0; i < kBinCount; ++i)
        {
            flush_bin(i);
        }
    }

   private:
    // 缓存链表节点: 复用"空闲块用户区前 8 字节" (块在缓存中, 用户区无数据,
    // 与空闲链表复用前 16 字节的思路一致, 零额外开销)
    struct CacheEntry
    {
        block_t* next;
    };

    static CacheEntry* entry_of(block_t* node)
    {
        return reinterpret_cast<CacheEntry*>(reinterpret_cast<char*>(node) + sizeof(block_t));
    }

    block_t* bins_[kBinCount] = {};  // 每档链表头 (块头指针)
    size_t counts_[kBinCount] = {};  // 每档当前缓存块数
    DrainFn drain_;                  // 倒回回调 (由装配点注入)
};

}  // namespace wageco
#endif  // MMEMORY_TCACHE_H
