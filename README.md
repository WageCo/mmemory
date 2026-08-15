# MMEMORY

A simple stack memory allocator. 教学演示用简易内存分配器。

底层用了 2 个双向循环链表,分别表示**已分配**以及**释放**的空间:分配时优先复用空闲链表中的块(first-fit + 分割),不够再向操作系统要;释放时合并物理相邻的空闲块,堆顶的块直接归还操作系统。

实属逆向优化 :D —— 它把真实 malloc 的核心机制(系统调用申请内存、块头元数据、链表管理、碎片合并)用最少代码演示清楚,但不做任何性能优化。

## 原理速览

```
内存布局 (每个块):
  [ header_t (16B: size + pre/next 指针) | 用户可用数据区 (size 字节) ]
  返回给用户的指针 = header 之后的位置

分配 malloc(n):
  1. 总占用 = (16 + n) 向上对齐到 16 字节
  2. 空闲链表 first-fit 找一块够大的
       ├─ 命中: 块过大则 split 分割, 剩余部分放回空闲链表
       └─ 未命中: sbrk() 向操作系统申请新堆空间
  3. 挂到已分配链表, 返回用户区指针

释放 free(p):
  1. 指针回退 16 字节拿到 header, 校验 (防 double free / 非法指针)
  2. 从已分配链表摘除
  3. 合并物理相邻的空闲块 (前驱/后继), 缓解碎片
  4. 位于堆顶 → sbrk(-) 归还 (连带回收下方连续空闲块)
     否则 → 挂回空闲链表等待复用

calloc = malloc + 清零;  realloc = 扩容时新分配 + 拷贝 + 释放旧块
```

## 目录结构

```
.
├── CMakeLists.txt
├── include/mmemory.h       # 对外接口 (wageco::malloc/free/calloc/realloc)
├── libs/
│   ├── googletest/         # 子模块 (系统无 gtest 时兜底)
│   ├── benchmark/          # 子模块 (系统无 benchmark 时兜底)
│   └── spdlog/             # 子模块 (系统无 spdlog 时兜底)
├── src/mmemory.cpp         # 分配器实现 (sbrk + 双向循环链表)
└── test/
    ├── unittest_malloc.cpp # gtest 单元测试
    └── bench_malloc.cpp    # google benchmark 对比基准
```

## 构建与测试

依赖:Linux + CMake ≥ 3.20 + C++14 编译器。

googletest / google-benchmark / spdlog **优先使用系统安装版**(`find_package`);系统没有时,CMake 直接引入仓库内子模块(`add_subdirectory`)。**CMake 不负责拉取子模块**:子模块未初始化时会给出提示,请手动执行 `git submodule update --init --recursive`(或 clone 时加 `--recurse-submodules`)。googletest / spdlog 缺失时构建报错并提示;benchmark 缺失时跳过基准目标,不阻塞测试构建。

```bash
# 克隆 (含子模块)
git clone --recurse-submodules https://github.com/your-name/mmemory.git
cd mmemory

# 或已克隆后手动初始化子模块
git submodule update --init --recursive

# 构建 + 测试 + 基准
cmake -S . -B build
cmake --build build -j$(nproc)
./build/CustomMemory          # 单元测试
./build/CustomMemory_bench    # 性能对比基准
```

> 提示:Windows + WSL2 场景,建议在 WSL 内构建(把仓库 clone 到 WSL 自己的文件系统,避免 `/mnt/c` 的 9P 桥接性能损耗)。

## 测试结果

环境:WSL2 Ubuntu, gcc 15.2.0, googletest 1.17.0

```
[==========] Running 4 tests from 1 test suite.
[----------] 4 tests from testMalloc
[ RUN      ] testMalloc.MallocTest                [       OK ] (0 ms)
[ RUN      ] testMalloc.MyMallocTest              [       OK ] (55 ms)
[ RUN      ] testMalloc.ReallocTest               [       OK ] (0 ms)
[ RUN      ] testMalloc.FragmentationShrinkTest   [       OK ] (3170 ms)
[----------] 4 tests from testMalloc (3226 ms total)
[  PASSED  ] 4 tests.
```

| 测试 | 验证内容 |
|---|---|
| MallocTest | 系统 malloc 压力测试(3.2M 次分配/释放) |
| MyMallocTest | 自定义分配器同模式压力测试 |
| ReallocTest | realloc 扩容内容保留 + `realloc(ptr, 0)` 释放语义 |
| FragmentationShrinkTest | 20000 块乱序释放后堆完整回收(碎片合并验证) |

## Benchmark 结果(自定义分配器 vs 系统 malloc)

> **环境与复现**:以下数据来自本项目开发机实测,`数值随 CPU / 编译器 / 系统库版本变化`,仅供量级参考。
> 复现方式:
> ```bash
> cmake -S . -B build && cmake --build build -j$(nproc)
> ./build/CustomMemory_bench --benchmark_min_time=0.1s
> ```
> 实测环境:WSL2 Ubuntu, 22 × 2.995 GHz, gcc 15.2.0, google-benchmark, 单次采样 0.1s

```
Benchmark                                   Time             CPU   Iterations
-----------------------------------------------------------------------------
BM_System_MallocFree/8                    4.98 ns         4.98 ns     27240005
BM_System_MallocFree/64                   5.21 ns         5.21 ns     27761039
BM_System_MallocFree/512                  5.23 ns         5.23 ns     26692512
BM_System_MallocFree/4096                 13.5 ns         13.5 ns     11014715
BM_Custom_MallocFree/8                    5794 ns         5795 ns        25060
BM_Custom_MallocFree/64                   5733 ns         5733 ns        25653
BM_Custom_MallocFree/512                  5551 ns         5550 ns        25530
BM_Custom_MallocFree/4096                 5914 ns         5886 ns        24251
BM_System_Batch/8                         4269 ns         4269 ns        32343
BM_System_Batch/64                        4141 ns         4141 ns        36015
BM_System_Batch/512                       3899 ns         3900 ns        34311
BM_System_Batch/4096                    198884 ns       198646 ns          732
BM_Custom_Batch/8                        74144 ns        73857 ns         2005
BM_Custom_Batch/64                       80583 ns        80251 ns         1831
BM_Custom_Batch/512                     107032 ns       106723 ns         1000
BM_Custom_Batch/4096                    359677 ns       359562 ns          375
BM_System_MallocFree_MT/8/threads:1       5.04 ns         5.04 ns     27579469
BM_System_MallocFree_MT/8/threads:4       5.09 ns         5.08 ns     23534700
BM_Custom_MallocFree_MT/8/threads:1       5425 ns         5425 ns        25932
BM_Custom_MallocFree_MT/8/threads:4       7768 ns         2974 ns        41700
```

### 结果解读

| 场景 | 系统 malloc | wageco | 差距 |
|---|---|---|---|
| 单次分配+释放 (8B) | 4.98 ns | 5,794 ns | ~**1100×** |
| 单次分配+释放 (4096B) | 13.5 ns | 5,914 ns | ~438× |
| 批量 256×8B | 4,269 ns | 74,144 ns | ~17× |
| 批量 256×4096B | 198,884 ns | 359,677 ns | ~1.8× |
| 多线程 4 线程 (8B) | 5.08 ns | 7,768 ns(总) | 并发无扩展 |

差距来自三处"每次必付"的固定成本:

1. **每次分配都可能 `sbrk` 系统调用**(µs 级);系统 malloc 命中 tcache 后零系统调用;
2. **释放时 O(n) 链表遍历**(合法性校验 + 前后邻居查找 ×2),块越多越慢;
3. **全局互斥锁**:4 线程下总耗时反而上升(5425→7768 ns),并发被串行化——系统 malloc 用 per-thread arena/tcache 规避了这点。

批量场景差距缩小(合并与堆顶回收摊薄了 sbrk 次数),大块(4096B)最接近——此时系统 malloc 也走 mmap/大块路径,双方成本结构相似。

> 这正是教学意义所在:这些数字直观展示了真实分配器为什么要做 **tcache / per-thread arena / 无锁设计**。

## 日志

使用 spdlog,默认输出到 **stderr**。级别分层(教学设计):

| 级别 | 内容 |
|---|---|
| `error` | 失败路径:malloc/sbrk 失败、double free、calloc 溢出、sbrk 收缩失败 |
| `debug` | 主流程:malloc 请求/复用/新块、free 入口、realloc 扩容 |
| `trace` | 细节:块分割(split)、前后邻居合并(coalesce)、堆顶连带回收、挂回空闲链表、calloc/realloc 数据操作 |

- `error` 始终可见;`debug` 默认不显示(`DEBUG` 编译时默认显示);`trace` 需显式开启;
- **编译期剥离**:日志用 `SPDLOG_LOGGER_*` 宏 + `SPDLOG_ACTIVE_LEVEL` 控制——
  Release 构建下 `debug`/`trace` 在编译期完全消除(参数不求值、零开销),
  保证 benchmark 测量的是纯分配器逻辑;查看 `debug`/`trace` 日志需 `-DDEBUG` 构建
  (如 `cmake -DCMAKE_CXX_FLAGS=-DDEBUG`);`error` 日志两种构建都有;
- 环境变量可覆盖:
  - `MMEMORY_LOG_LEVEL` — `trace|debug|info|warn|error|critical|off`
  - `MMEMORY_LOG_FILE=<path>` — 指定时改为追加模式写文件;未指定则输出到 stderr
- 使用 `stderr_logger_mt` / `basic_logger_mt` 工厂函数创建 logger
  (注意: spdlog 1.17 起 `stderr_logger_mt` 声明在 `sinks/stdout_sinks.h`,
  `basic_logger_mt` 声明在 `sinks/basic_file_sink.h`;
  1.17 不再提供 `stderr_sinks.h` / `stderr_color_sinks.h`)

```bash
# 最全日志 (trace 级别 + 落地文件), 可直接观察到分割/合并/回收全过程
MMEMORY_LOG_LEVEL=trace MMEMORY_LOG_FILE=/tmp/mmemory.log ./build/CustomMemory
```

```bash
# 查看分配/释放日志并落地到文件
MMEMORY_LOG_LEVEL=debug MMEMORY_LOG_FILE=/tmp/mmemory.log ./build/CustomMemory
```

## 已知限制

- 仅支持 Linux(`sbrk` + `pthread`,Windows/MSVC/MinGW 无法编译);
- 单全局锁,多线程无扩展性;
- 链表 O(n) 查找,无 bin 分级 / 哈希索引;
- `realloc` 缩容不分割(原地返回,空间不回收);
- 教学实现,不追求与系统 malloc 的性能可比性。
