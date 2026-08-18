# MMEMORY

A simple memory allocator. 自我学习用简易内存分配器。

个人逆向学习之作:通过亲手实现一个简易分配器,把真实 malloc 的核心机制(系统调用申请内存、块元数据、空闲块复用、碎片合并)用最少代码吃透,并实践**依赖注入架构**:分配器 = **存储模式 + 内存申请 + 查找策略** 三个可注入依赖的组合,每个维度都可替换。

> **本分支 (template_c++11)**:编译期多态版 —— 用 C++ 模板 (header-only) 实现
> 同一个分配器,删除 master 分支的全部虚函数接口 (`IList`/`IMemory`/
> `IFindStrategy`),依赖通过模板参数编译期注入,并用上模板独有的编译期
> 能力 (能力 trait 分派、SFINAE 接口约束、host_traits 宿主泛型化)。
> 算法逻辑与 master 完全一致,差异在"运行时多态 vs 编译期多态",
> 详见"设计"章节。

实属逆向优化 :D —— 自我学习为主,不求性能,只求把原理学明白。

## 原理

```
内存布局 (每个块, 物理解耦/边界 tag 思路, 同 dlmalloc):
  [ block_t (16B: size + inuse) | 用户可用数据区 (size 字节) ]
  已分配: 用户区全是数据
  空闲:   用户区前 16B 复用为链表节点 ListNode (零节点开销)
  返回给用户的指针 = block_t 之后的位置
  没有"已分配链表": 块是否已分配由 inuse 标志标识

每线程快路径 (tcache, 同 glibc):
  [ 线程本地缓存 (16B 一档, 16~1024B 共 64 档, 每档最多 7 块) ]
  小对象 malloc: 先查本线程缓存 → 命中零锁/零系统调用/零链表查找
  小对象 free:   先压入本线程缓存 → 档满时整档倒回全局分配器
  (缓存只延迟回收: 倒回后参与合并与反向释放, 不阻止回收)

分配 malloc(n):
  0. n <= 1024: 先查线程本地缓存 (精确档位命中即返回, 无需 split)
  1. 总占用 = (16 + n) 向上对齐到 16 字节
  2. 按注入的查找策略 (FirstFit/BestFit) 在空闲链表中找一块够大的
       ├─ 命中: 块过大则 split 分割, 剩余部分放回空闲链表
       └─ 未命中: 通过内存提供者 (IMemory/sbrk) 申请新堆空间
  3. 标记 inuse=true, 返回用户区指针

释放 free(p):
  0. 用户区大小 <= 1024: 校验 (owns_address + inuse) 后压入线程本地缓存
  1. 校验: 地址在提供者空间内 (owns_address) + inuse 标志 (防 double free)
  2. 合并物理相邻的空闲块 (链表查找, 不假设堆形状)
  3. 释放委托给内存提供者 (IMemory):
       - 支持随机释放 (如 mmap) → 直接归还
       - 否则 (如 sbrk 栈式) → 按"申请顺序反向释放": 只有物理上贴住
         当前边界的块能归还, 成功后连带其下方紧邻空闲块继续反向归还;
         失败则挂回空闲链表复用

calloc = malloc + 清零 (含溢出检查);  realloc = 扩容时新分配 + 拷贝 + 释放旧块
```

## 设计

### 架构与依赖注入

分配器 = **存储模式 + 内存申请 + 查找策略** 三个可注入依赖的组合。

> **本分支 (template_c++11) 为编译期多态版**:master 分支用三个虚接口
> (`IList`/`IMemory`/`IFindStrategy`) 做**运行时**依赖注入;本分支删除全部
> 虚接口,依赖通过**模板参数编译期注入**(实现全部 header-only)。
> 算法逻辑与 master 完全一致,但模板版进一步用上了**模板独有的编译期能力**
> (见下"模板真正用在哪"):编译期能力分派、编译期接口约束、宿主泛型化。

```
ListNode               链表节点 (pre/next, 独立定义; 复用空闲块用户区前 16B)
   ↑
HeaderList<HostT>      泛型双向循环链表模板 (唯一的一条"空闲链表")
                       HostT = 宿主类型 (如 block_t); 节点↔宿主互转/取大小/
                       头大小由 host_traits<HostT> 编译期适配 —— 换宿主只
                       需特化 host_traits, 链表代码零改动

SbrkMemory              基于 sbrk/brk 的内存提供者 (普通类, 方法非虚)
  ├─ allocate(size)                       申请
  ├─ release_block(addr, size)            归还一块 (随机/反向由提供者决定)
  ├─ owns_address(addr)                   地址是否在本提供者空间内 (free 校验)
  └─ 能力由 memory_traits<SbrkMemory> 编译期声明: 非随机释放
     (取代运行时 supports_random_release(), 释放路径编译期分派)

查找策略 (普通 struct, find 是模板成员函数, 编译期鸭子类型)
  ├─ FirstFit : 第一个 size>=需求 的块 (快)
  └─ BestFit  : size>=需求 且最小 的块 (碎片最小, 全表扫描)

Tcache<kMaxBytes, kBinLimit>  线程本地小对象缓存模板 (公共 API 前的快路径)
  ├─ 档位 = 对齐后用户区大小: 16/32/.../kMaxBytes, 每档最多 kBinLimit 块
  ├─ 命中: 零锁、零系统调用、零链表查找 (malloc/free 都是 O(1))
  ├─ 档满: 整档倒回全局分配器 (缓存只延迟回收, 不阻止回收)
  └─ 编译期约束: 模板参数非法 (非 16 倍数/为 0) 直接编译报错

functional.h            编译期函数式层 (constexpr 纯函数 + 表生成)
  ├─ align_up / block_total / align_user_size / bin_of   对齐与档位纯函数
  ├─ BinSizeAt<Bin>      类型级递归 (模板特化)
  ├─ bin_of_recursive    值级递归 (constexpr 递归函数)
  └─ kReqToBin           编译期生成 size→bin 查找表 (static_assert 校验)
     ↑ Allocator::malloc / Tcache::bin_of_request 已接入纯函数

Allocator<ListT, MemoryT, StrategyT>   ← 三个依赖编译期注入 (模板实参)
   ↑ 编译期接口约束 (SFINAE + static_assert): 依赖类型必须提供关键成员
wageco::malloc/free/calloc/realloc    ← tcache 快路径 + 转发到全局 g_allocator
```

**物理解耦(边界 tag 思路,同 dlmalloc)**:

- `block_t`(16B)只存 `size + inuse`,不内嵌链表节点;
- **空闲块**的用户区前 16 字节复用为 `ListNode`(空闲块没有用户数据,节点零开销);**已分配块**用户区全给数据;
- 因此没有"已分配链表":`free` 用 `inuse` + `owns_address` 校验,物理相邻的空闲块通过链表查找定位。

**可替换性(换依赖 = 换模板实参,分配器零改动)**:

- 换存储模式(如按大小分 bin)→ 提供新的 `ListT`(符合接口形状即可),组合根换模板实参;
- 换内存策略(如 mmap 大块映射,支持随机释放)→ 提供新的 `MemoryT` + 特化 `memory_traits<MemoryT>`(编译期声明能力),组合根换模板实参;
- 换查找策略(FirstFit ↔ BestFit)→ 组合根换模板实参即可;
- `src/mmemory.cpp` 是唯一装配点(组合根,模板实例化)。

### 模板真正用在哪(本分支与"机械替换"的区别)

模板不只是把虚函数调用换成模板调用——它用上了三个运行时多态做不到的
编译期能力:

1. **编译期能力分派 (tag dispatch)**:"是否支持随机释放"是提供者类型的
   固有属性,由 `memory_traits<MemoryT>::random_release` 编译期声明;
   `Allocator::free` 用 `release_dispatch(..., std::integral_constant<...>)`
   在编译期选择释放路径 —— 未选中的路径**不会被实例化**(死代码消除)。
   master 版是运行时 `if (supports_random_release())`,两个分支都存在;
2. **编译期接口约束 (SFINAE + static_assert)**:`Allocator` 的模板参数是
   "编译期鸭子类型",用 SFINAE 检测依赖类型是否提供 `allocate`/
   `release_block`/`owns_address`/`find_first_fit`/`insert`/`remove`,
   不满足时 `static_assert` 给出可读中文诊断 (如 "MemoryT 必须提供:
   void* allocate(size_t)"),而不是一屏模板实例化错误;
3. **宿主泛型化 (host_traits)**:`HeaderList<HostT>` 只认识 `ListNode`,
   如何把节点解释为宿主块由 `host_traits<HostT>` 编译期适配 ——
   同一份链表代码可服务任意宿主 (测试里有 24B 头的 `demo_block` 演示:
   仅特化 `host_traits` 就复用全部链表功能),这是模板代码生成能力;
4. **编译期参数与约束**:`Tcache<kMaxBytes, kBinLimit>` 的档数、档位计算
   全部编译期求值,非法参数直接编译报错。

这些是"模板版 vs 虚函数版"教学对比的实质:运行时多态把能力留到运行期
查询、把约束留到运行期失败;编译期多态把两者都提前到编译期。

**线程本地缓存 (tcache)**:公共 API 与全局分配器之间的一层 per-thread 快路径
(见 `include/tcache.h`,模板 `Tcache<kMaxBytes, kBinLimit>`,默认 1024B/7 块),
实现 glibc 的核心优化:

- 小对象(请求 ≤ kMaxBytes)的 `malloc`/`free` 先命中本线程缓存:档位按"对齐后
  的用户区大小"精确匹配(16B 一档),出缓存无需 split,命中路径零锁、
  零系统调用、零空闲链表查找;
- 块在缓存中 `inuse=false`(空闲),倒回全局分配器前恢复 `inuse=true`;
- 每档最多 kBinLimit 块(默认 7,glibc 默认),满了把整档倒回全局分配器——
  缓存只延迟回收,不阻止回收(倒回后参与物理合并与反向释放);
- 线程私有(thread_local 实例),快路径完全无锁,小对象场景不再被全局锁串行化;
- `flush_tcache()`:把当前线程缓存中的块全部倒回全局分配器(退出/测试用);
- Allocator 类保持"存储+内存+策略"三依赖纯净,tcache 只是公共 API 前的加速层。

**断点缓存**:独占堆契约下本库是唯一 sbrk 使用者,`SbrkMemory` 缓存当前断点
(`cur_break_`,由 allocate/release_block 同步维护),`owns_address` 校验只做
内存比较,不在热路径调用 `sbrk(0)`。

### 编译期函数式(见 `include/functional.h`)

函数式编程在分配器里的适用边界:**计算层(无状态、输入→输出)适合纯函数式;
内核(链表/指针/锁)是可变状态机,不适合**。本头在计算层演示三种函数式
形态,全部编译期求值、零运行时开销,每个函数都配 `static_assert` 编译期
单元测试(算错直接编译失败):

1. **constexpr 纯函数族**:`align_up` / `block_total` / `align_user_size` /
   `bin_of` / `bin_user_size` —— 对齐与档位的输入→输出函数,`Allocator::malloc`
   与 `Tcache::bin_of_request` 已接入(`block_total(size)` 替代手写位运算,
   行为逐位一致);
2. **类型级递归(模板特化)**:`BinSizeAt<Bin>` —— 编译期"递归函数",
   基准情形 + 递归情形,展示"类型即函数"的元编程形态;
3. **值级递归(constexpr 递归)**:`bin_of_recursive` —— 递归即循环的函数式
   写法(无循环变量、无可变状态,状态通过参数传递);
4. **编译期表生成**:`make_req_to_bin_table()` 用 constexpr 纯函数构造
   `size→bin` 查找表(请求 0..1024 → 档位),编译期求值后进只读段,
   `static_assert` 静态校验表内容——这是"数据即函数"形态,也是
   size-class bin 分级的预演(将来做 bin 表直接复用这套手法)。

> 接入方式:对齐/档位计算全部收敛到纯函数,`Tcache` 的 `bin_of_request`
> 与表在 `CompileTimeTableTest` 中全量比对一致(编译期生成的数据与
> 运行时计算殊途同归)。

## 测试结果

> 仅性能基准(wageco vs 系统 malloc);单元测试由 CI 保证,提交即通过,不在此罗列。

> **环境与复现**:以下数据来自本项目开发机实测,`数值随 CPU / 编译器 / 系统库版本 / 机器负载变化`,仅供量级参考。
> 复现:
> ```bash
> ./build/CustomMemory_bench --benchmark_min_time=0.1s
> ./build/CustomMemory_bench_system --benchmark_min_time=0.1s
> ```
> 实测环境:WSL2 Ubuntu, 22 × 2.995 GHz, gcc 15.2.0, google-benchmark, 单次采样 0.1s

```
CustomMemory_bench (wageco, 模板版, 已接入 tcache):
Benchmark                            Time             CPU   Iterations
----------------------------------------------------------------------
BM_AllocFree/8                    13.3 ns         13.3 ns     10714078
BM_AllocFree/64                   13.9 ns         13.9 ns      9599930
BM_AllocFree/512                  13.4 ns         13.4 ns      9099900
BM_AllocFree/4096                 4073 ns         4073 ns        33119
BM_Batch/8                       12845 ns        12845 ns        10355
BM_Batch/64                      13360 ns        13360 ns        10771
BM_Batch/512                     84372 ns        83782 ns         1601
BM_Batch/4096                   685923 ns       685946 ns          198
BM_AllocFree_MT/8/threads:1       12.9 ns         12.9 ns     10792899
BM_AllocFree_MT/8/threads:4       32.1 ns         32.1 ns      3789048
BM_RandomSize                      244 ns          244 ns       616272
BM_AllocHold/64                   9.03 ns         9.03 ns     15151720
BM_AllocHold/1024                 9.07 ns         9.07 ns     16913001
BM_Churn                          23.5 ns         23.5 ns      5603856

CustomMemory_bench_system (系统 malloc):
Benchmark                            Time             CPU   Iterations
----------------------------------------------------------------------
BM_AllocFree/8                    11.9 ns         11.9 ns     10933968
BM_AllocFree/64                   12.1 ns         12.1 ns     11494824
BM_AllocFree/512                  12.2 ns         12.2 ns     11881889
BM_AllocFree/4096                 27.8 ns         27.8 ns      5121112
BM_Batch/8                        9132 ns         9132 ns        14875
BM_Batch/64                       8773 ns         8774 ns        15736
BM_Batch/512                      8470 ns         8470 ns        16294
BM_Batch/4096                   471880 ns       471647 ns          302
BM_AllocFree_MT/8/threads:1       12.5 ns         12.5 ns     11131507
BM_AllocFree_MT/8/threads:4       11.7 ns         11.7 ns     11523212
BM_RandomSize                     39.2 ns         39.2 ns      3580567
BM_AllocHold/64                   8.98 ns         8.99 ns     15109000
BM_AllocHold/1024                 8.84 ns         8.84 ns     15072922
BM_Churn                          21.8 ns         21.8 ns      6343411
```

> MT 行 (BM_AllocFree_MT) 在低负载下单独采样 (负载高时 4 线程 wall 可涨到
> 30~60 ns, 见下文"结果解读"中的假共享说明)。
> 本分支 (template_c++11) 与 master 的差异仅在"编译期模板注入 vs 运行时
> 虚函数注入",算法完全相同;同条件下 benchmark 同量级 (模板版 Batch 小对象
> 场景因消除虚函数与 std::function 开销略快),以下数据量级参考即可。

### 结果解读

| 场景 | 系统 malloc | wageco (模板版, 接入 tcache) | 接入前 | 差距变化 |
|---|---|---|---|---|
| 单次分配+释放 (8B) | 11.9 ns | 13.3 ns | 9,520 ns | ~1870× → **持平** |
| 单次分配+释放 (4096B) | 27.8 ns | 4,073 ns | 9,237 ns | ~670× → ~146× |
| 批量 256×8B | 9,132 ns | 12,845 ns | 34,752 ns | ~8.6× → ~1.4× |
| 批量 256×4096B | 471,880 ns | 685,923 ns | 320,895 ns | ~1.7× → ~1.5× |
| 随机大小 (LCG) | 39.2 ns | 244 ns¹ | 5,798 ns | ~320× → ~6× |
| Churn (随机交替) | 21.8 ns | 23.5 ns | 2,889 ns | ~300× → ~1.1× |
| 稳态占用 AllocHold/64 | 8.98 ns | 9.03 ns | 33.9 ns | ~8.4× → **持平** |
| 多线程 4 线程 (8B) | 11.7 ns | 32.1 ns | 8,794 ns(总) | 无扩展 → **有扩展** |

> ¹ 随机大小场景对机器负载极敏感 (低负载时可到 ~57 ns, 与系统 malloc 持平),
> 表中为采样时负载 ~1.0 的值, 量级参考。

tcache 命中路径(零锁、零系统调用、零链表查找)把"每次必付"的固定成本
(µs 级 sbrk + 全局锁 + O(n) 查找)从热路径上彻底移除:

1. **单次分配+释放小对象反超系统 malloc**:8B/64B/512B 都到 ~13 ns,与系统
   malloc 持平甚至略快——这正是 tcache 的全部意义(对应接入前 ~1870× 差距);
2. **随机/Churn 场景差距缩到 ~1~6×**:一半左右请求命中缓存,未命中部分仍付
   全局锁 + 链表查找成本(对应接入前 ~300× 差距);随机大小对负载极敏感,
   低负载时与系统 malloc 持平;
3. **批量小对象差距缩到 ~1.4×**:每档只缓存 7 块,批量 256 块时频繁"档满整档
   倒回"(每次倒回 = 7 次带锁的全局 free),倒回成为新热点;系统 tcache 同样
   有该机制,但其分 bin 空闲链表让倒回更便宜——这是下一项"size-class bin"
   要解决的;
4. **大块 4096 仍 ~146× 落后**:4096 > 1024B 缓存上限,不走 tcache,每次
   malloc/free 仍付 sbrk + 锁 + 链表查找;系统 malloc 的大块路径有 mmap 与
   独立缓存,成本结构完全不同 (perf 实测:该场景 ~99% 耗时是 sbrk 系统调用);
5. **多线程不再串行化**:每线程独立缓存,8B 场景 4 线程 wall 32 ns (每线程
   CPU ~8 ns),接入前 4 线程总耗时反而上升 (4671→8794 ns)。残余的 wall
   膨胀 (~2.5×) 来自共享堆上相邻线程块的 block_t 头**假共享**(各线程从同一
   sbrk 堆申请,块地址相邻,pop/push 写 inuse 标志互相 invalidate 缓存行)
   与偶尔的档满倒回(取全局锁)——这正是下一项"per-thread arena"要解决的;
6. **AllocHold 稳态占用与系统持平**:缓存命中已是 O(1),9 ns 与系统 malloc
   相当。

> 对比接入前:单次分配+释放从 9,520 ns 降到 ~13 ns (~700× 提速),随机大小从
> 5,798 ns 降到 ~60 ns 量级 (低负载),Churn 从 2,889 ns 降到 ~24 ns (~120×)。
> 差距从"系统 malloc 的数百倍"缩到"同一数量级",剩余的每一项差距都对应
> ROADMAP 中一项具体优化,继续逐项验证即可。
> 模板版 (本分支) 与 master 虚函数版同条件下性能同量级——差异 (编译期 vs
> 运行时绑定) 被系统调用与锁成本淹没,这正是教学实验想展示的结论之一:
> **依赖注入方式的选择对分配器性能影响有限,真正的瓶颈在系统调用与锁**。

## 日志

基于 spdlog,默认输出到 stderr。级别自低到高:`trace`(分割/合并细节)→ `debug`(主流程)→ `info`(DEBUG 构建:退出时泄漏检测/统计)→ `error`(失败路径)。

- **编译期剥离**:`debug`/`trace` 在 Release 构建下编译期消除,零开销,保证 benchmark 测的是纯分配器逻辑;查看需 `-DDEBUG` 构建(如 `cmake -DCMAKE_CXX_FLAGS=-DDEBUG`);`error` 两种构建始终可见;
- **环境变量**:`MMEMORY_LOG_LEVEL`(`trace|debug|info|warn|error|critical|off`)、`MMEMORY_LOG_FILE=<path>`(落地文件,默认 stderr)。

```bash
# trace 级别 + 落地文件, 可观察分割/合并/回收全过程
MMEMORY_LOG_LEVEL=trace MMEMORY_LOG_FILE=/tmp/mmemory.log ./build/CustomMemory_bench
```

## 已知限制

- **sbrk/brk 独占堆**:默认内存提供者 `SbrkMemory` 使用 `sbrk`,操作的是**进程级全局断点**——
  断点被外部移动会导致反向释放错乱。因此本库**不适合与系统 malloc 混用**。两种正确用法:
  1. **独占假设(默认)**:只用 `wageco::malloc/free` 等命名空间 API,进程内不使用系统 malloc / 其他 sbrk 使用者(自用学习);
  2. **链接期接管(强制互斥)**:构建时 `-DMMEMORY_OVERRIDE_MALLOC=ON`,库导出 `__wrap_malloc/free/calloc/realloc`,
     可执行文件链接 `-Wl,--wrap=malloc,...` —— 进程内**所有** `malloc` 调用(含第三方库)统一重定向到本库,
     与系统 malloc 在链接期互斥,不存在混用(tcmalloc/jemalloc 的替换机制);
- 仅支持 Linux(`sbrk` + `pthread`,Windows/MSVC/MinGW 无法编译);
- **单全局锁 + 共享堆**:tcache 命中路径已无锁,但缓存未命中/档满倒回仍走全局
  锁;多线程 wall 时间仍有假共享膨胀(见"结果解读"),per-thread arena 是下一步;
- 链表 O(n) 查找,无 bin 分级 / 哈希索引;
- **tcache 只覆盖 ≤ 1024B 的小对象**,大块每次仍付 sbrk + 锁 + 链表查找;
- `calloc`/`realloc` 未接入 tcache 快路径(直接走全局分配器,教学取舍);
- `realloc` 缩容不分割(原地返回,空间不回收);
- 学习实现,不追求与系统 malloc 的性能可比性。

## 未来计划(优化方向)

见 [ROADMAP.md](ROADMAP.md)—— 自我学习路线图,每项对应一个已知限制或 benchmark 差距。
