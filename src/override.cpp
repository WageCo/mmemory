// ============================================================================
// override.cpp - 链接期接管系统 malloc (独占堆模式)
// ----------------------------------------------------------------------------
// 配合 CMake 选项 -DMMEMORY_OVERRIDE_MALLOC=ON 使用, 链接时给可执行文件加:
//     -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc
// 效果:
//   - 进程内所有对 malloc/free/calloc/realloc 的调用 (用户代码、第三方库、
//     甚至 operator new 底层) 都被链接器重定向到 __wrap_* → wageco 分配器
//     (sbrk 独占堆);
//   - 与系统 malloc 在链接期互斥: 链接本模式后整个进程统一走本库,
//     不存在"混用" —— 这正是 tcmalloc / jemalloc 替换 malloc 的机制。
// 注意: __wrap_* 内部调用 wageco::malloc 等"限定名"调用, 不会再次被 wrap,
//       避免递归。
// ============================================================================
#include <stddef.h>

#include "mmemory.h"

extern "C"
{
    void* __wrap_malloc(size_t size) { return wageco::malloc(size); }
    void __wrap_free(void* ptr) { wageco::free(ptr); }
    void* __wrap_calloc(size_t num, size_t size) { return wageco::calloc(num, size); }
    void* __wrap_realloc(void* ptr, size_t size) { return wageco::realloc(ptr, size); }
}
