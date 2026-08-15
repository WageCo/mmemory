#!/usr/bin/env bash
# ============================================================================
# ci-local.sh - 在本地模拟 GitHub Actions CI (近似 .github/workflows/ci.yml)
# ----------------------------------------------------------------------------
# 作用: 在干净环境里走一遍 CI 的完整流程, 提前发现构建/测试/基准问题。
#
# 注意: 本脚本【不拉取子模块】, 依赖系统安装的 googletest / google-benchmark
#       (与 CMake 行为一致: CMake 只 add 子模块, 不负责拉取)。
#       若系统缺库, 请手动: sudo apt install libgtest-dev libbenchmark-dev
#       或 git submodule update --init --recursive。
#
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

echo "==> 1/5 干净环境: $WORK (源: $SRC)"
rm -rf "$WORK"

echo "==> 2/5 git clone (等价 actions/checkout@v4)"
git clone "$SRC" "$WORK"
cd "$WORK"

echo "==> 3/5 cmake configure + build (依赖系统库, CMake 不拉取子模块)"
cmake -S . -B build
cmake --build build -j"$(nproc)"

echo "==> 4/5 单元测试"
./build/CustomMemory

echo "==> 5/5 快速基准"
if [ -x ./build/CustomMemory_bench ]; then
    ./build/CustomMemory_bench --benchmark_min_time=0.1s
else
    echo "==> (CustomMemory_bench 未构建, 跳过; 系统缺 benchmark 库)"
fi

echo "==> CI 本地模拟通过 ✅"
