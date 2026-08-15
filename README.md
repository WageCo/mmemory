# MMEMORY

A simple memory allocator. 自我学习用简易内存分配器。

个人逆向学习之作:通过亲手实现一个简易分配器,把真实 malloc 的核心机制(系统调用申请内存、块元数据、空闲块复用、碎片合并)用最少代码吃透,并实践**依赖注入架构**:分配器 = **存储模式 + 内存申请 + 查找策略** 三个可注入依赖的组合,每个维度都可替换。

实属逆向优化 :D —— 自我学习为主,不求性能,只求把原理学明白。

## 原理速览

```
内存布局 (每个块, 物理解耦/边界 tag 思路, 同 dlmalloc):
  [ block_t (16B: size + inuse) | 用户可用数据区 (size 字节) ]
  已分配: 用户区全是数据
  空闲:   用户区前 16B 复用为链表节点 ListNode (零节点开销)
  返回给用户的指针 = block_t 之后的位置
  没有"已分配链表": 块是否已分配由 inuse 标志标识

分配 malloc(n):
  1. 总占用 = (16 + n) 向上对齐到 16 字节
  2. 按注入的查找策略 (FirstFit/BestFit) 在空闲链表中找一块够大的
       ├─ 命中: 块过大则 split 分割, 剩余部分放回空闲链表
       └─ 未命中: 通过内存提供者 (IMemory/sbrk) 申请新堆空间
  3. 标记 inuse=true, 返回用户区指针

释放 free(p):
  1. 校验: 地址在提供者空间内 (owns_address) + inuse 标志 (防 double free)
  2. 合并物理相邻的空闲块 (链表查找, 不假设堆形状)
  3. 释放委托给内存提供者 (IMemory):
       - 支持随机释放 (如 mmap) → 直接归还
       - 否则 (如 sbrk 栈式) → 按"申请顺序反向释放": 只有物理上贴住
         当前边界的块能归还, 成功后连带其下方紧邻空闲块继续反向归还;
         失败则挂回空闲链表复用

calloc = malloc + 清零 (含溢出检查);  realloc = 扩容时新分配 + 拷贝 + 释放旧块
```

## 目录结构

```
.
├── CMakeLists.txt
├── include/                     # 头文件统一放这里 (内部头非公共接口)
│   ├── mmemory.h                # 对外接口 (wageco::malloc/free/calloc/realloc)
│   ├── logging.h                # 日志配置 (SPDLOG_ACTIVE_LEVEL) + get_logger
│   ├── list.h                   # 链表层: ListNode / IList / HeaderList
│   ├── block.h                  # 块层: block_t / 块-节点互转 helpers
│   ├── memory.h                 # 内存提供者层: IMemory / SbrkMemory
│   ├── find_strategy.h          # 查找策略层: IFindStrategy / FirstFit / BestFit
│   ├── allocator.h              # 分配器层: Allocator
│   └── internal.h               # 聚合总头 (src/*.cpp 使用)
├── libs/
│   ├── benchmark/               # 子模块 (系统无 benchmark 时兜底)
│   └── spdlog/                  # 子模块 (系统无 spdlog 时兜底)
├── src/
│   ├── mmemory.cpp              # 组合根: 装配依赖 + 4 个公共 API 转发
│   ├── allocator.cpp            # Allocator: 存储模式×内存申请×查找策略 的组合逻辑
│   ├── list.cpp                 # HeaderList: 双向循环链表 (存储模式实现)
│   ├── memory.cpp               # SbrkMemory: sbrk/brk 封装 (内存申请实现)
│   ├── log.cpp                  # 日志系统实现 (spdlog)
│   └── override.cpp             # 链接期接管系统 malloc (仅 MMEMORY_OVERRIDE_MALLOC=ON)
└── test/
    └── bench_malloc.cpp         # google benchmark 对比基准 (单份代码, 宏切换命名空间)
```

## 架构与依赖注入

分配器 = **存储模式 + 内存申请 + 查找策略** 三个可注入依赖的组合(策略模式):

```
ListNode               链表节点 (pre/next, 独立定义; 复用空闲块用户区前 16B)
   ↑
IList                  存储模式抽象接口 (insert/remove/find_first_fit/contains/物理相邻/for_each)
   ↑ 继承
HeaderList : IList     双向循环链表 (唯一的一条"空闲链表"; 通过 SizeFn 回调取块大小)

IMemory                内存提供者能力契约 (任意来源: sbrk/mmap/池)
  ├─ allocate(size)                       申请
  ├─ supports_random_release()            是否支持随机释放
  ├─ release_block(addr, size)            归还一块 (随机/反向由提供者决定)
  └─ owns_address(addr)                   地址是否在本提供者空间内 (free 校验)
   ↑ 继承
SbrkMemory              基于 sbrk/brk 的实现 (反向释放: 物理贴边界才归还)

IFindStrategy           空闲块查找策略
  ├─ FirstFit : 第一个 size>=需求 的块 (快)
  └─ BestFit  : size>=需求 且最小 的块 (碎片最小, 全表扫描)

Allocator(IList*, IMemory*, IFindStrategy*)   ← 三个依赖注入
   ↑
wageco::malloc/free/calloc/realloc    ← 转发到全局 g_allocator
```

**物理解耦(边界 tag 思路,同 dlmalloc)**:

- `block_t`(16B)只存 `size + inuse`,不内嵌链表节点;
- **空闲块**的用户区前 16 字节复用为 `ListNode`(空闲块没有用户数据,节点零开销);**已分配块**用户区全给数据;
- 因此没有"已分配链表":`free` 用 `inuse` + `owns_address` 校验,物理相邻的空闲块通过链表查找定位。

**可替换性**:

- 换存储模式(如按大小分 bin)→ 提供新的 `IList` 实现,分配器零改动;
- 换内存策略(如 mmap 大块映射,支持随机释放)→ 提供新的 `IMemory` 实现,分配器零改动;
- 换查找策略(FirstFit ↔ BestFit)→ 组合根换一个对象即可;
- `src/mmemory.cpp` 是唯一装配点(组合根)。

## 构建与基准测试

依赖:Linux + CMake ≥ 3.20 + C++14 编译器。

google-benchmark / spdlog **优先使用系统安装版**(`find_package`);系统没有时,CMake 直接引入仓库内子模块(`add_subdirectory`)。**CMake 不负责拉取子模块**:子模块未初始化时会给出提示,请手动执行 `git submodule update --init --recursive`(或 clone 时加 `--recurse-submodules`)。benchmark 缺失时跳过基准目标,不阻塞其余构建。

```bash
# 克隆 (含子模块)
git clone --recurse-submodules https://github.com/WageCo/mmemory.git
cd mmemory

# 构建
cmake -S . -B build
cmake --build build -j$(nproc)
```

产出**两个基准可执行文件**(单份代码,宏 `MMEMORY_TEST_CUSTOM` 切换命名空间):

```bash
# 性能基准 (两者输出并排对比)
./build/CustomMemory_bench --benchmark_min_time=0.1s
./build/CustomMemory_bench_system --benchmark_min_time=0.1s
```

> 本库独占堆、与系统 malloc 互斥,因此"系统对照"与"本库基准"拆成独立可执行文件(进程级隔离),避免相互干扰。可选链接期接管:`cmake -S . -B build -DMMEMORY_OVERRIDE_MALLOC=ON`(见"已知限制")。

> 提示:Windows + WSL2 场景,建议在 WSL 内构建(把仓库 clone 到 WSL 自己的文件系统,避免 `/mnt/c` 的 9P 桥接性能损耗)。

## Benchmark 结果(wageco vs 系统 malloc)

> **环境与复现**:以下数据来自本项目开发机实测,`数值随 CPU / 编译器 / 系统库版本变化`,仅供量级参考。
> 复现:
> ```bash
> ./build/CustomMemory_bench --benchmark_min_time=0.1s
> ./build/CustomMemory_bench_system --benchmark_min_time=0.1s
> ```
> 实测环境:WSL2 Ubuntu, 22 × 2.995 GHz, gcc 15.2.0, google-benchmark, 单次采样 0.1s

```
CustomMemory_bench (wageco):
Benchmark                            Time             CPU   Iterations
----------------------------------------------------------------------
BM_AllocFree/8                    9520 ns         9520 ns        15469
BM_AllocFree/64                  10682 ns        10683 ns        10000
BM_AllocFree/512                  9581 ns         9581 ns        13255
BM_AllocFree/4096                 9237 ns         9238 ns        12257
BM_Batch/8                       34752 ns        34752 ns         3910
BM_Batch/64                      39692 ns        39693 ns         3634
BM_Batch/512                     74135 ns        74137 ns         1950
BM_Batch/4096                   320895 ns       320883 ns          500
BM_AllocFree_MT/8/threads:1       4671 ns         4671 ns        31498
BM_AllocFree_MT/8/threads:4       8794 ns         3444 ns        32876
BM_RandomSize                     5798 ns         5798 ns        25307
BM_AllocHold/64                   33.9 ns         33.9 ns      4247850
BM_AllocHold/1024                 33.7 ns         33.8 ns      4334267
BM_Churn                          2889 ns         2889 ns        44684

CustomMemory_bench_system (系统 malloc):
Benchmark                            Time             CPU   Iterations
----------------------------------------------------------------------
BM_AllocFree/8                    5.08 ns         5.08 ns     27645353
BM_AllocFree/64                   4.98 ns         4.98 ns     28535463
BM_AllocFree/512                  4.95 ns         4.95 ns     28001886
BM_AllocFree/4096                 13.8 ns         13.8 ns      9860635
BM_Batch/8                        4022 ns         4022 ns        34855
BM_Batch/64                       4071 ns         4071 ns        35327
BM_Batch/512                      3815 ns         3815 ns        35093
BM_Batch/4096                   185810 ns       185663 ns          742
BM_AllocFree_MT/8/threads:1       4.91 ns         4.91 ns     28242964
BM_AllocFree_MT/8/threads:4       5.35 ns         5.34 ns     26538348
BM_RandomSize                     17.9 ns         17.9 ns      8611617
BM_AllocHold/64                   4.04 ns         4.04 ns     34900138
BM_AllocHold/1024                 4.11 ns         4.11 ns     37172336
BM_Churn                          9.73 ns         9.73 ns     11466786
```

### 结果解读

| 场景 | 系统 malloc | wageco | 差距 |
|---|---|---|---|
| 单次分配+释放 (8B) | 5.08 ns | 9,520 ns | ~**1870×** |
| 单次分配+释放 (4096B) | 13.8 ns | 9,237 ns | ~670× |
| 批量 256×8B | 4,022 ns | 34,752 ns | ~8.6× |
| 批量 256×4096B | 185,810 ns | 320,895 ns | ~1.7× |
| 随机大小 (LCG) | 17.9 ns | 5,798 ns | ~320× |
| Churn (随机交替) | 9.73 ns | 2,889 ns | ~300× |
| 稳态占用 AllocHold/64 | 4.04 ns | 33.9 ns | ~8.4× |
| 多线程 4 线程 (8B) | 5.35 ns | 8,794 ns(总) | 并发无扩展 |

差距来自"每次必付"的固定成本:

1. **每次分配都可能 `sbrk` 系统调用**(µs 级);系统 malloc 命中 tcache 后零系统调用;
2. **释放时的校验与合并**(`owns_address` + 空闲链表物理相邻查找),块越多越慢;
3. **全局互斥锁**:4 线程下总耗时反而上升(4671→8794 ns),并发被串行化——系统 malloc 用 per-thread arena/tcache 规避了这点。

差距缩小/特殊的场景:

- **批量场景差距缩小到 ~2-9×**:批量释放触发合并与反向释放,`sbrk` 次数摊薄;大块(4096B)最接近——此时系统 malloc 也走 mmap/大块路径,双方成本结构相似;
- **AllocHold 稳态占用 wageco 只要 ~34 ns**:保持 1024 块存活、每轮替换一个,空闲链表稳定命中(first-fit 快速路径),不再每次 `sbrk`——说明"复用为主"的稳态场景差距大幅缩小。

> 这正是学习意义所在:这些数字直观展示了真实分配器为什么要做 **tcache / per-thread arena / 无锁设计**,也展示了"空闲复用为主"时分配器可以接近系统水平。

## 日志

使用 spdlog,默认输出到 **stderr**。级别分层(学习设计):

| 级别 | 内容 |
|---|---|
| `error` | 失败路径:malloc 过大/失败、double free、calloc 溢出、释放失败 |
| `debug` | 主流程:malloc 请求/复用/新块、free 入口、realloc 扩容 |
| `trace` | 细节:块分割(split)、前后邻居合并(coalesce)、挂回空闲链表、calloc/realloc 数据操作 |
| `info` | DEBUG 构建:退出时泄漏检测 / 分配统计 / 提供者字节统计 |

- `error` 始终可见;`debug` 默认不显示(`DEBUG` 编译时默认显示);`trace` 需显式开启;
- **编译期剥离**:日志用 `SPDLOG_LOGGER_*` 宏 + `SPDLOG_ACTIVE_LEVEL` 控制——
  Release 构建下 `debug`/`trace` 在编译期完全消除(参数不求值、零开销),
  保证 benchmark 测量的是纯分配器逻辑;查看 `debug`/`trace` 日志需 `-DDEBUG` 构建
  (如 `cmake -DCMAKE_CXX_FLAGS=-DDEBUG`);`error` 日志两种构建都有;
- **进程生命周期 logger**:`get_logger()` 返回空删除器别名,logger 永不析构,
  因此程序退出时(全局对象析构阶段)的泄漏检测/统计打印依然安全;
- 环境变量可覆盖:
  - `MMEMORY_LOG_LEVEL` — `trace|debug|info|warn|error|critical|off`
  - `MMEMORY_LOG_FILE=<path>` — 指定时改为追加模式写文件;未指定则输出到 stderr
- 使用 `stderr_logger_mt` / `basic_logger_mt` 工厂函数创建 logger
  (注意: spdlog 1.17 起 `stderr_logger_mt` 声明在 `sinks/stdout_sinks.h`,
  `basic_logger_mt` 声明在 `sinks/basic_file_sink.h`;
  1.17 不再提供 `stderr_sinks.h` / `stderr_color_sinks.h`)

```bash
# 最全日志 (trace 级别 + 落地文件), 可直接观察到分割/合并/回收全过程
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
- 单全局锁,多线程无扩展性;
- 链表 O(n) 查找,无 bin 分级 / 哈希索引;
- `realloc` 缩容不分割(原地返回,空间不回收);
- 学习实现,不追求与系统 malloc 的性能可比性。

## 未来计划(优化方向)

见 [ROADMAP.md](ROADMAP.md)—— 自我学习路线图,每项对应一个已知限制或 benchmark 差距。
