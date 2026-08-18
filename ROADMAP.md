# 未来计划(优化方向)

自我学习路线图——每一项都对应 [README](README.md) 里某个已知限制或 benchmark 差距,按"先易后难、先单线程后多线程"排序:

- [x] **tcache(线程本地缓存)**:每次分配/释放先命中本线程小缓存,零系统调用、零加锁。
      这是 benchmark 中 ~1870× 差距的最大来源,也是 glibc 的核心优化;
      (已实现: `include/tcache.h`(模板分支为 `Tcache<kMaxBytes, kBinLimit>`),
      公共 API 前的 per-thread 快路径,16B 一档共 64 档、每档 7 块;
      8B 单次分配+释放 9,520 ns → ~13 ns)
- [x] **编译期多态对照(template_c++11 分支)**:用 C++ 模板重写同一分配器,
      删除虚接口 (IList/IMemory/IFindStrategy),依赖模板参数编译期注入,
      header-only;并深入用上模板独有能力:memory_traits 编译期能力分派
      (tag dispatch, 未选路径不实例化)、SFINAE+static_assert 接口约束、
      host_traits 宿主泛型化 (HeaderList<HostT> 可服务任意宿主);
      与 master 形成"运行时多态 vs 编译期多态"的教学对比
      (实测同量级:依赖注入方式对性能影响有限,瓶颈在系统调用与锁);
- [x] **编译期函数式 (functional.h)**:把计算层 (对齐/档位) 收敛为 constexpr
      纯函数, 用模板递归/特化 + constexpr 纯函数在编译期生成 size→bin 查找表
      (static_assert 编译期单元测试), 为 size-class bin 预演; 明确了
      函数式编程在分配器里的适用边界 (计算层适合, 可变状态内核不适合);
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

> 每完成一项,更新 README 中"测试结果"的 Benchmark 表格,直观看到差距如何一步步缩小。
> 目标不是超越系统 malloc,而是把每个优化背后的原理亲手验证一遍。
