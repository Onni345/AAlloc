#!/usr/bin/env bash
# bench_runner.sh — build and run all benchmarks for aalloc vs glibc,
# then write results to build/bench_results.txt
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
BENCH="$ROOT/benchmarks"
OUT="$BUILD/bench_results.txt"

cd "$ROOT"

echo "=== AAlloc Benchmark Runner ===" | tee "$OUT"
echo "Date: $(date)" | tee -a "$OUT"
echo "Host: $(uname -srm)" | tee -a "$OUT"
echo "" | tee -a "$OUT"

# Build libraries first
echo "[1/2] Building libaalloc..." | tee -a "$OUT"
make -s all

# Build benchmark binaries
echo "[2/2] Building benchmarks..." | tee -a "$OUT"
make -s bench

run_pair() {
    local name="$1"
    local bin_aa="$BUILD/${name}_aa"
    local bin_sys="$BUILD/${name}_sys"

    echo "" | tee -a "$OUT"
    echo "────────────────────────────────────────────────────────────" | tee -a "$OUT"
    echo "  $name" | tee -a "$OUT"
    echo "────────────────────────────────────────────────────────────" | tee -a "$OUT"

    echo "" | tee -a "$OUT"
    "$bin_aa" 2>&1 | tee -a "$OUT"

    echo "" | tee -a "$OUT"
    "$bin_sys" 2>&1 | tee -a "$OUT"
}

run_pair bench_throughput
run_pair bench_latency
run_pair bench_fragmentation

echo "" | tee -a "$OUT"
echo "Results saved to $OUT"
