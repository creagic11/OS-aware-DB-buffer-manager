#!/usr/bin/env bash
set -e

echo "=== 1. Building BufferScope ==="
ninja -C build

echo ""
echo "=== 2. Running Full Unit Test Suite ==="
cd build
ctest --output-on-failure
cd ..

echo ""
echo "=== 3. Running Buffered I/O Baseline Benchmark ==="
./build/bench_runner --ops 50000 --keys 1000 --runs 5

echo ""
echo "=== BufferScope Phase 4 Verification Complete ==="
