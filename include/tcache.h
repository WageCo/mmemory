// ============================================================================
// tcache.h - 线程本地缓存层 (Tcache, 对应 ROADMAP 第一项)
// ----------------------------------------------------------------------------
// 在"公共 API → 全局分配器"之间插入一层 per-thread 小对象缓存 (tcache):
//   - 只缓存"小对象": 对齐后的用户区大小 (block_t::head.size) <= kMaxBytes
//     (默认 1024B), 16 字节一档共 64 档 (16B ~ 1024B);
//   - 命中缓存: 零锁、零系统调用、零空闲链表查找 —— malloc/free 都是 O(1),
//     这是 glibc 的核心优化, 也是 README benchmark 中 ~1870× 差距的最大来源;
//   - 档位按"对齐后的用户区大小"精确匹配 (与 block_t::head.size 一致),
//     因此出缓存时必然正好满足请求, 无需 split;
//   - 每档最多 kBinLimit 块 (默认 7, glibc 的 tcache_count 默认值),
//     满了把整档倒回全局分配器 (倒回后参与合并与反向释放, 缓存只延迟回收);
//   - 线程私有: 实例为 thread_local, 只被本线程访问, 快路径完全无锁 ——
//     顺带缓解了"单全局锁, 多线程无扩展性"的限制 (小对象场景);
//   - 块在缓存中 inuse=false (空闲); 倒回全局分配器前恢复 inuse=true
//     (分配器的 free 用 inuse 标志校验)。
// 实现位置: src/tcache.cpp; 装配 (thread_local 实例 + 倒回回调) 见 src/mmemory.cpp。
// 说明: 本层只服务于"公共 API" (wageco::malloc/free), Allocator 类本身
//       保持"存储模式 + 内存申请 + 查找策略"三依赖纯净, 不受影响。
// ============================================================================
#ifndef MMEMORY_TCACHE_H
#define MMEMORY_TCACHE_H

#include <stddef.h>  // size_t

#include "block.h"  // block_t / align_to

namespace wageco
{
class Tcache
{
   public:
    // 缓存上限: 用户区大小 (block_t::head.size) 不超过该值才进缓存
    static constexpr size_t kMaxBytes = 1024;
    // 每档步长 = 对齐粒度 16B; 档数 = 1024/16 = 64 (16B ~ 1024B)
    static constexpr size_t kBinCount = kMaxBytes / align_to;
    // 每档最多缓存的块数 (glibc tcache 的默认 count)
    static constexpr size_t kBinLimit = 7;

    // 倒回回调: 把一块缓存块交还给全局分配器 (由装配点注入, 见 src/mmemory.cpp)
    using DrainFn = void (*)(block_t* node);

    explicit Tcache(DrainFn drain) : drain_(drain) {}

    // 请求大小 size 是否走缓存 (size <= kMaxBytes 时, 对齐后的用户区大小
    // 必落在 16~kMaxBytes 档内, 且对齐运算不可能溢出)
    static bool covers(size_t size) { return size <= kMaxBytes; }
    // 请求 size -> 档位索引 (前提: covers(size); 对齐后用户区 16B->0, 32B->1, ...)
    static size_t bin_of_request(size_t size);
    // 块头用户区大小 (head.size, 恒为 16 倍数) -> 档位索引
    static size_t bin_of_block(size_t user_size);

    // 从档位弹出一块 (缓存命中), 无则返回 nullptr; 命中后 inuse 置回 true
    void* pop(size_t bin);
    // 压入一块 (调用方需已把 inuse 置为 false); 档满返回 false
    bool push(block_t* node);
    // 档位是否已满
    bool full(size_t bin) const { return counts_[bin] >= kBinLimit; }
    // 当前缓存块总数 (DEBUG/统计用)
    size_t count() const;

    // 把某一档全部缓存块倒回全局分配器 (drain_ 回调), 清空该档
    void flush_bin(size_t bin);
    // 把全部档倒回全局分配器 (程序退出/显式调用/测试)
    void flush_all();

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
