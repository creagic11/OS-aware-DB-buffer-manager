#!/usr/bin/env bash
# BufferScope — the single documented command sequence.
#
#   scripts/run_all.sh            build, test, full benchmark matrix, plots, validation
#   scripts/run_all.sh --quick    same pipeline with 10x smaller workloads and 3 runs
#                                 (a smoke check of the pipeline; NOT for reporting)
#   scripts/run_all.sh --no-bench build + tests only
#
# Environment overrides: BUILD_DIR (build), DATA_DIR (data/bench), RESULTS_DIR (results),
# ONLY=e1_working_set,e3_hotspot (subset of experiments).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD_DIR="${BUILD_DIR:-build}"
DATA_DIR="${DATA_DIR:-data/bench}"
RESULTS_DIR="${RESULTS_DIR:-results}"
MODE="full"
case "${1:-}" in
  --quick) MODE="quick" ;;
  --no-bench) MODE="nobench" ;;
  "") ;;
  *) echo "usage: $0 [--quick|--no-bench]"; exit 2 ;;
esac

step() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }

step "0. Checking prerequisites"
missing=()
for tool in cmake ninja g++ python3; do command -v "$tool" >/dev/null || missing+=("$tool"); done
python3 -c 'import pandas, matplotlib, tomllib' 2>/dev/null || missing+=("python3: pandas/matplotlib (python>=3.11)")
if ((${#missing[@]})); then
  echo "missing: ${missing[*]}"
  echo "Ubuntu: sudo apt install build-essential cmake ninja-build libgtest-dev libbenchmark-dev python3-pandas python3-matplotlib sysstat"
  exit 1
fi
FSTYPE="$(mkdir -p "$DATA_DIR" && findmnt -n -o FSTYPE -T "$DATA_DIR" 2>/dev/null || echo unknown)"
echo "data dir: $DATA_DIR (filesystem: $FSTYPE)"
if [[ "$FSTYPE" == "tmpfs" || "$FSTYPE" == "overlay" ]]; then
  echo "WARNING: $FSTYPE does not exercise the block-device page-cache path (and may reject O_DIRECT)."
  echo "         Set DATA_DIR to a directory on ext4/xfs for meaningful results."
fi

step "1. Configure + build (Release)"
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"

step "2. Full test suite"
ctest --test-dir "$BUILD_DIR" --output-on-failure

[[ "$MODE" == "nobench" ]] && { echo "done (benchmarks skipped)"; exit 0; }

step "3. Benchmark matrix"
EXTRA=()
[[ "$MODE" == "quick" ]] && EXTRA+=(--quick --runs 3)
[[ -n "${ONLY:-}" ]] && EXTRA+=(--only "$ONLY")
python3 scripts/run_experiments.py --build "$BUILD_DIR" --data "$DATA_DIR" --out "$RESULTS_DIR" "${EXTRA[@]}"

step "4. Plots from raw CSV"
python3 scripts/plot.py --results "$RESULTS_DIR"

step "5. Validate results (CSV <-> plot consistency, variance, >= 5 experiments)"
MIN=5
[[ -n "${ONLY:-}" ]] && MIN=1
python3 scripts/check_results.py --results "$RESULTS_DIR" --min-experiments "$MIN"

echo
echo "All done. Raw data: $RESULTS_DIR/raw  plots: $RESULTS_DIR/plots  tables: $RESULTS_DIR/summary"
