# 未来计划(优化方向)

自我学习路线图——每一项都对应 [README](README.md) 里某个已知限制或 benchmark 差距,按"先易后难、先单线程后多线程"排序:

- [ ] **tcache(线程本地缓存)**:每次分配/释放先命中本线程小缓存,零系统调用、零加锁。
      这是 benchmark 中 ~1870× 差距的最大来源,也是 glibc 的核心优化;
- [ ] **size-class bin 分级**:空闲链表按大小分桶(如 16/32/64/.../2KB),查找从 O(n)
      降到近似 O(1),替代单一 HeaderList(练习新 `IList` 实现,分配器零改动);
- [ ] **realloc 缩容分割**:缩容时把多余部分分割挂回空闲链表,回收空间;
- [ ] **mmap 大块提供者**:实现基于 `mmap` 的 `IMemory`(支持随机释放),
      顺便打通"大块走 mmap、小块走 sbrk"的经典两级策略;
- [ ] **per-thread arena / 无锁**:多线程下消除全局互斥锁串行化(对应
      `BM_AllocFree_MT` 4 线程总耗时反而上升的现象),学习 glibc 的 arena 思路;
- [ ] **Segregated Fit / 更优查找策略**:注入式加入第三种 `IFindStrategy`,
      对比 FirstFit / BestFit 的碎片与性能;
- [ ] **Windows 支持**:用 `VirtualAlloc` 替换 `sbrk`,验证依赖注入的跨平台能力
      (存储模式与查找策略无需改动);
- [ ] **健壮性工具链**:接入 AddressSanitizer / 模糊测试(fuzz),把边界 case
      (溢出、double free、碎片极限)系统性跑一遍;
- [ ] **学习笔记沉淀**:把每个优化点的原理、实现取舍、benchmark 前后对比
      整理成文档(如 `docs/`),让"为什么真实分配器要这么做"可追溯。

> 每完成一项,更新 README 中的 Benchmark 表格,直观看到差距如何一步步缩小。
> 目标不是超越系统 malloc,而是把每个优化背后的原理亲手验证一遍。
