// ============================================================================
// mmemory.h - 教学用简易内存分配器对外接口
// ----------------------------------------------------------------------------
// 提供与标准 C 库同名的四个函数, 位于 namespace wageco:
//
//   wageco::malloc(size)    分配 size 字节, 返回 16 字节对齐的指针; 失败返回 NULL
//   wageco::free(addr)      释放 malloc/calloc/realloc 返回的指针; NULL 是合法参数
//   wageco::calloc(n, size) 分配 n*size 字节并清零; 溢出时返回 NULL
//   wageco::realloc(addr, n) 调整大小; 语义与标准 realloc 一致
//
// 实现基于 sbrk() + 双向循环链表, 详见 src/mmemory.cpp。
// 注意: 仅支持 Linux (依赖 <unistd.h> 的 sbrk 与 pthread)。
//
// 日志 (spdlog, 环境变量可配置):
//   MMEMORY_LOG_LEVEL=trace|debug|info|warn|error|critical|off
//     - error: 失败路径 (malloc 失败 / double free / calloc 溢出等)
//     - debug: 分配/释放主流程 (DEBUG 编译时默认开启)
//     - trace: 分割 / 合并 / 回收等细节
//   MMEMORY_LOG_FILE=<path>  指定时改为追加模式写文件, 否则输出 stderr
// ============================================================================
#ifndef __WAGECO_MMEMORY_H_
#define __WAGECO_MMEMORY_H_
#include <unistd.h>
namespace wageco
{
void *malloc(size_t size);
void free(void *addr);
void *calloc(size_t num, size_t size);
void *realloc(void *addr, size_t size);
} // namespace wageco

#endif
