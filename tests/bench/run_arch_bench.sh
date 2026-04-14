#!/bin/bash
# tests/bench/run_arch_bench.sh — Native benchmark: amd64 vs arm64 (Graviton)
#
# Runs bench_pipeline, bench_simd_jit, bench_sql, bench_parallel, bench_hdb
# on both architectures and compares results.
#
# Usage:
#   ./tests/bench/run_arch_bench.sh                    # full run (sync + build + bench)
#   ./tests/bench/run_arch_bench.sh --skip-build       # skip build (use existing binaries)
#   ./tests/bench/run_arch_bench.sh --bench pipeline   # run single bench
#
# Prerequisites:
#   - Graviton instance at GRAVITON_HOST with SSH key
#   - Both machines have clang-19, cmake, ninja
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GRAVITON_HOST="${GRAVITON_HOST:-172.31.71.135}"
GRAVITON_KEY="${GRAVITON_KEY:-$HOME/ec2-jinmp.pem}"
SSH="ssh -i $GRAVITON_KEY -o StrictHostKeyChecking=no ec2-user@$GRAVITON_HOST"
SKIP_BUILD=false
BENCH_FILTER="all"
RESULT_DIR="/tmp/arch_bench_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULT_DIR"

BENCHES=(bench_pipeline bench_simd_jit bench_sql bench_parallel bench_hdb)

GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
step()  { echo -e "\n${CYAN}══ $* ══${NC}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) SKIP_BUILD=true; shift ;;
    --bench)      BENCH_FILTER="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 [--skip-build] [--bench NAME]"
      echo "  --skip-build  Use existing binaries"
      echo "  --bench NAME  Run single bench (pipeline|simd_jit|sql|parallel|hdb)"
      exit 0 ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

[ "$BENCH_FILTER" != "all" ] && BENCHES=("bench_$BENCH_FILTER")

# ═══════════════════════════════════════════════════════════
# 1. Sync code to Graviton
# ═══════════════════════════════════════════════════════════
step "1/4 Sync code to Graviton"
rsync -az --delete \
  -e "ssh -i $GRAVITON_KEY -o StrictHostKeyChecking=no" \
  --exclude='build/' --exclude='.git/' --exclude='node_modules/' \
  --exclude='web/out/' --exclude='web/.next/' --exclude='__pycache__/' \
  "$PROJECT_ROOT/" "ec2-user@${GRAVITON_HOST}:~/zeptodb/"
info "Synced"

# ═══════════════════════════════════════════════════════════
# 2. Build on both architectures
# ═══════════════════════════════════════════════════════════
if ! $SKIP_BUILD; then
  step "2/4 Build benchmarks"

  BENCH_TARGETS="${BENCHES[*]}"

  info "Building on amd64 (local)..."
  cd "$PROJECT_ROOT/build"
  ninja -j$(nproc) $BENCH_TARGETS 2>&1 | tail -3
  info "amd64 build done"

  info "Building on arm64 (Graviton)..."
  $SSH "cd ~/zeptodb && mkdir -p build && cd build && \
    cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
      -DZEPTO_USE_S3=OFF -DZEPTO_USE_PARQUET=OFF -DZEPTO_USE_FLIGHT=OFF \
      -DZEPTO_BUILD_PYTHON=OFF -DAPEX_USE_RDMA=OFF -DZEPTO_USE_UCX=OFF \
      -DZEPTO_USE_JIT=ON -DAPEX_USE_IO_URING=ON 2>&1 | tail -3 && \
    ninja -j\$(nproc) $BENCH_TARGETS 2>&1 | tail -3"
  info "arm64 build done"
else
  info "Skipping build (--skip-build)"
fi

# ═══════════════════════════════════════════════════════════
# 3. Run benchmarks
# ═══════════════════════════════════════════════════════════
step "3/4 Run benchmarks"

for BENCH in "${BENCHES[@]}"; do
  echo ""
  info "── $BENCH ──"

  # amd64
  info "  amd64..."
  timeout 120 "$PROJECT_ROOT/build/$BENCH" > "$RESULT_DIR/${BENCH}_amd64.log" 2>&1 || true

  # arm64
  info "  arm64..."
  $SSH "timeout 120 ~/zeptodb/build/$BENCH" > "$RESULT_DIR/${BENCH}_arm64.log" 2>&1 || true
done

# ═══════════════════════════════════════════════════════════
# 4. Compare results
# ═══════════════════════════════════════════════════════════
step "4/4 Results"

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║     Native Benchmark: amd64 vs arm64 (Graviton)         ║"
echo "╚══════════════════════════════════════════════════════════╝"

for BENCH in "${BENCHES[@]}"; do
  echo ""
  echo "━━━ $BENCH ━━━"
  echo ""

  AMD_LOG="$RESULT_DIR/${BENCH}_amd64.log"
  ARM_LOG="$RESULT_DIR/${BENCH}_arm64.log"

  case "$BENCH" in
    bench_simd_jit)
      echo "  Metric                          amd64           arm64"
      echo "  ─────────────────────────────────────────────────────"
      # Extract SIMD lines with speedup
      paste <(grep -E "^\[SIMD\]" "$AMD_LOG" 2>/dev/null | head -9) \
            <(grep -E "^\[SIMD\]" "$ARM_LOG" 2>/dev/null | head -9) | \
      while IFS=$'\t' read -r amd arm; do
        label=$(echo "$amd" | awk '{print $2}' | head -c 20)
        rows=$(echo "$amd" | grep -oP '\d+[KM] rows' | head -1)
        amd_simd=$(echo "$amd" | grep -oP 'simd=\s*\K\d+')
        arm_simd=$(echo "$arm" | grep -oP 'simd=\s*\K\d+')
        if [ -n "$amd_simd" ] && [ -n "$arm_simd" ]; then
          printf "  %-18s %6s  %8sμs  %8sμs\n" "$label" "$rows" "$amd_simd" "$arm_simd"
        fi
      done
      ;;

    bench_sql)
      echo "  Query                           amd64 avg       arm64 avg"
      echo "  ─────────────────────────────────────────────────────────"
      paste <(grep -E "^\[" "$AMD_LOG" 2>/dev/null | grep "avg=" | head -10) \
            <(grep -E "^\[" "$ARM_LOG" 2>/dev/null | grep "avg=" | head -10) | \
      while IFS=$'\t' read -r amd arm; do
        label=$(echo "$amd" | grep -oP '^\[\K[^\]]+' | sed 's/ *$//')
        amd_avg=$(echo "$amd" | grep -oP 'avg=\s*\K[\d.]+')
        arm_avg=$(echo "$arm" | grep -oP 'avg=\s*\K[\d.]+')
        if [ -n "$amd_avg" ] && [ -n "$arm_avg" ]; then
          printf "  %-35s %8sμs  %8sμs\n" "$label" "$amd_avg" "$arm_avg"
        fi
      done
      ;;

    bench_pipeline)
      echo "  amd64:"
      grep -iE "ticks/sec|throughput|latency|ingest" "$AMD_LOG" 2>/dev/null | head -10 | sed 's/^/    /'
      echo "  arm64:"
      grep -iE "ticks/sec|throughput|latency|ingest" "$ARM_LOG" 2>/dev/null | head -10 | sed 's/^/    /'
      ;;

    *)
      echo "  amd64:"
      grep -iE "μs|ms|sec|speedup|throughput|latency|req/s|ticks|rows" "$AMD_LOG" 2>/dev/null | head -15 | sed 's/^/    /'
      echo "  arm64:"
      grep -iE "μs|ms|sec|speedup|throughput|latency|req/s|ticks|rows" "$ARM_LOG" 2>/dev/null | head -15 | sed 's/^/    /'
      ;;
  esac
done

echo ""
echo "Full logs: $RESULT_DIR/"
info "Done"
