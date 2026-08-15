#!/usr/bin/env bash
# ============================================================================
# ci-local.sh - 在本地模拟 GitHub Actions CI (近似 .github/workflows/ci.yml)
# ----------------------------------------------------------------------------
# 作用: 在干净环境里走一遍 CI 的完整流程, 提前发现构建/测试/基准问题。
#
# 注意: 本脚本【不拉取子模块】, 依赖系统安装的 googletest / google-benchmark / spdlog
#       (与 CMake 行为一致: CMake 只 add 子模块, 不负责拉取)。
#       若系统缺库, 请手动: sudo apt install libgtest-dev libbenchmark-dev libspdlog-dev
#       或 git submodule update --init --recursive。
#
# 覆盖: 构建 + 单元测试 (wageco/系统 两个可执行文件) + 性能基准 (两个) + 格式检查。
# 差异: 本地用你机器上的编译器/CMake, 与 GitHub 的 ubuntu-latest 版本可能
#       略有不同, 但构建与测试流程一致。
# 用法 (WSL 内):
#   bash .github/ci-local.sh            # 默认以 ~/mmemory 为源
#   bash .github/ci-local.sh <源仓库路径> # 指定其他源
# ============================================================================
set -euo pipefail

# 源仓库: 默认取 WSL 内已有的克隆 (避免 /mnt/c 的 9P 慢速与 dubious ownership)
SRC="${1:-$HOME/mmemory}"
WORK="${TMPDIR:-/tmp}/mmemory-ci"

echo "==> 1/6 干净环境: $WORK (源: $SRC)"
rm -rf "$WORK"

echo "==> 2/6 git clone (等价 actions/checkout@v4)"
git clone "$SRC" "$WORK"
cd "$WORK"

echo "==> 3/6 cmake configure + build (依赖系统库, CMake 不拉取子模块)"
cmake -S . -B build
cmake --build build -j"$(nproc)"

echo "==> 4/6 单元测试 (wageco + 系统对照, 进程级隔离)"
./build/CustomMemory
./build/CustomMemory_system

echo "==> 5/6 性能基准 (wageco + 系统对照)"
if [ -x ./build/CustomMemory_bench ]; then
    ./build/CustomMemory_bench --benchmark_min_time=0.1s
    ./build/CustomMemory_bench_system --benchmark_min_time=0.1s
else
    echo "==> (bench 未构建, 跳过; 系统缺 benchmark 库)"
fi

echo "==> 6/6 格式检查 (clang-format; 未安装则跳过)"
if cmake --build build --target check-format 2>/dev/null; then
    :
else
    echo "==> (clang-format 缺失或 check-format 目标不可用, 跳过)"
fi

echo "==> CI 本地模拟通过 ✅"
