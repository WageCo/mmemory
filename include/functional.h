// ============================================================================
// functional.h - 编译期函数式工具层 (constexpr 纯函数 + 模板递归 + 表生成)
// ----------------------------------------------------------------------------
// 函数式编程在分配器里的适用边界:
//   - 计算层 (对齐/档位/表, 无状态、输入->输出) 非常适合纯函数式;
//   - 内核 (链表/指针/锁) 是可变状态机, 不适合 (硬套函数式只会引入
//     拷贝与间接调用, 破坏零开销)。
// 本头演示三种函数式形态 (全部编译期求值, 零运行时开销):
//   1) constexpr 纯函数族: align_up / block_total / align_user_size /
//      bin_of / bin_user_size —— 输入->输出, 无副作用, 编译期可求值;
//   2) 类型级递归 (模板特化): BinSizeAt<Bin> —— 编译期"递归函数";
//   3) 值级递归 (constexpr 递归函数): bin_of_recursive ——
//      基准情形 + 递归调用 (递归即循环的函数式形态);
//   4) 编译期表生成: make_req_to_bin_table() —— constexpr 纯函数构造
//      size->bin 查找表, 存只读段, 零运行时开销 (为 size-class bin 分级
//      预演: 将来做 bin 表可直接复用这套"编译期生成配置"的手法)。
// 每个函数/表都配 static_assert —— 编译期单元测试: 算错直接编译失败。
// ============================================================================
#ifndef MMEMORY_FUNCTIONAL_H
#define MMEMORY_FUNCTIONAL_H

#include <stddef.h>  // size_t

#include "block.h"  // block_t / align_to

namespace wageco
{
// ----------------------------------------------------------------------------
// 1. constexpr 纯函数族 (输入 -> 输出, 无副作用, 编译期可求值)
// ----------------------------------------------------------------------------
// 向上对齐: n 对齐到 align 的倍数。
// 注意: C++14 的 constexpr 函数体内不能用 static_assert 依赖函数参数,
// 因此"align 必须是 2 的幂"由调用方保证 (本库只有 align_to=16 一个调用点,
// 顶层 static_assert 已覆盖)。
constexpr size_t align_up(size_t n, size_t align) { return (n + align - 1) & ~(align - 1); }

// 编译期确认对齐粒度是 2 的幂 (align_up 的前提)
static_assert((align_to & (align_to - 1)) == 0, "align_to 必须是 2 的幂");

// 块总占用 = 16 对齐(block 头 + 用户区) (与 Allocator::malloc 公式一致)
constexpr size_t block_total(size_t user_size) { return align_up(sizeof(block_t) + user_size, align_to); }

// 请求大小 -> 对齐后用户区大小 (与 block_t::head.size 一致)
constexpr size_t align_user_size(size_t request) { return block_total(request) - sizeof(block_t); }

// 对齐后用户区大小 -> 档位 (16B->0, 32B->1, ..., 1024B->63)
constexpr size_t bin_of(size_t user_size) { return user_size / align_to - 1; }

// 档位 -> 该档用户区大小 (0->16, 1->32, ...)
constexpr size_t bin_user_size(size_t bin) { return (bin + 1) * align_to; }

// --- 编译期单元测试 (static_assert: 算错直接编译失败) ---
static_assert(align_up(1, 16) == 16, "align_up(1,16)");
static_assert(align_up(17, 16) == 32, "align_up(17,16)");
static_assert(block_total(64) == 80, "block_total(64)");
static_assert(align_user_size(64) == 64, "align_user_size(64)");
static_assert(align_user_size(100) == 112, "align_user_size(100)");
static_assert(bin_of(16) == 0, "bin_of(16)");
static_assert(bin_of(112) == 6, "bin_of(112)");
static_assert(bin_of(1024) == 63, "bin_of(1024)");
static_assert(bin_user_size(0) == 16, "bin_user_size(0)");
static_assert(bin_user_size(63) == 1024, "bin_user_size(63)");

// ----------------------------------------------------------------------------
// 2. 类型级递归 (模板特化) —— 编译期"递归函数"
// ----------------------------------------------------------------------------
// BinSizeAt<Bin>: 第 Bin 档的用户区大小。递归定义:
//   基准情形 Bin==0 -> 16; 递归情形 Bin>0 -> 16 + BinSizeAt<Bin-1>。
// (结果与 bin_user_size 等价, 但用模板递归展开 —— 展示"类型即函数"的
//  函数式元编程形态: 编译器在编译期展开递归)
template <size_t Bin>
struct BinSizeAt
{
    static constexpr size_t value = align_to + BinSizeAt<Bin - 1>::value;
};
template <>
struct BinSizeAt<0>
{
    static constexpr size_t value = align_to;
};
static_assert(BinSizeAt<0>::value == 16, "BinSizeAt<0>");
static_assert(BinSizeAt<3>::value == 64, "BinSizeAt<3>");
static_assert(BinSizeAt<63>::value == 1024, "BinSizeAt<63>");

// ----------------------------------------------------------------------------
// 3. 值级递归 (constexpr 递归函数) —— 基准情形 + 递归调用
// ----------------------------------------------------------------------------
// bin_of_recursive: 用户区大小 -> 档位, 递归定义 (教学演示形态;
// 实际用算术 bin_of O(1) 即可, 递归只为展示函数式"递归即循环"的写法:
// 无循环变量、无可变状态, 状态通过参数传递)
constexpr size_t bin_of_recursive(size_t user_size)
{
    return user_size <= align_to ? 0 : 1 + bin_of_recursive(user_size - align_to);
}
static_assert(bin_of_recursive(16) == 0, "bin_of_recursive(16)");
static_assert(bin_of_recursive(1024) == 63, "bin_of_recursive(1024)");
// 递归版与算术版殊途同归 (编译期验证一致性)
static_assert(bin_of_recursive(112) == bin_of(112), "recursive == arithmetic");

// ----------------------------------------------------------------------------
// 4. 编译期表生成 —— constexpr 纯函数构造 size->bin 查找表
// ----------------------------------------------------------------------------
// 请求大小 0..kMaxCachedBytes -> 档位 的查找表, 编译期求值后存只读段。
// 这是"数据即函数"的形态: 表本身是编译期生成的不可变数据。
constexpr size_t kMaxCachedBytes = 1024;                  // 与 Tcache 默认上限一致
constexpr size_t kReqToBinEntries = kMaxCachedBytes + 1;  // 请求 0..1024

// 表载体: 用 struct 包原生数组 (C++14 的 constexpr 函数可以写原生数组元素,
// 但不能写 std::array 的非 const operator[] —— 那是 C++17 才 constexpr 化)
struct BinLookupTable
{
    size_t values[kReqToBinEntries];
};

// constexpr 纯函数: 构造整张表 (输入无、输出表; 内部局部变量只在编译期存在)
constexpr BinLookupTable make_req_to_bin_table()
{
    BinLookupTable t{};
    for (size_t req = 0; req < kReqToBinEntries; ++req)
    {
        t.values[req] = req == 0 ? 0 : bin_of(align_user_size(req));
    }
    return t;
}
// 编译期求值: 表内容在编译期算好, 进入只读数据段, 运行时零开销
static constexpr BinLookupTable kReqToBin = make_req_to_bin_table();

// --- 静态校验表内容 (编译期单元测试) ---
static_assert(kReqToBin.values[0] == 0, "table[0]");
static_assert(kReqToBin.values[1] == 0, "table[1]");      // 请求 1 -> user 16 -> 档 0
static_assert(kReqToBin.values[64] == 3, "table[64]");    // 请求 64 -> user 64 -> 档 3
static_assert(kReqToBin.values[100] == 6, "table[100]");  // 请求 100 -> user 112 -> 档 6
static_assert(kReqToBin.values[1024] == 63, "table[1024]");

}  // namespace wageco
#endif  // MMEMORY_FUNCTIONAL_H
