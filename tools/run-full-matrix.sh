#!/usr/bin/env bash
# ============================================================================
# tools/run-full-matrix.sh — devlog 092 (+ 093 parallel arm64 SSH; 094 perf)
# ----------------------------------------------------------------------------
# Single orchestrator for the complete ZeptoDB test matrix:
#   1 build             — ninja test binaries + servers
#   2 unit_x86          — ctest parallel (excl. Benchmark.* and K8s* unit tests)
#   3 integration       — tests/integration/*.sh
#   4 python            — pytest tests/python/
#   5 bench_local       — tests/bench/run_arch_bench.sh (opt-in, local-only)
#   6 aarch64_unit      — tools/run-aarch64-tests.sh   (opt-in, EKS; DEPRECATED,
#                         use stage 8 instead — see devlog 094)
#   7 eks_full          — tests/k8s/run_eks_bench.sh  (opt-in, EKS)
#   8 aarch64_unit_ssh  — rsync+ssh+ninja+ctest on persistent Graviton host
#                         (default ON; runs in parallel with stages 2, 3, 4)
#
# Fail-fast by default. Stages 6+7 share a single EKS wake/sleep cycle.
# Stage 8 uses a persistent Graviton EC2 (no EKS); non-blocking if SSH fails.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TS="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="/tmp/zepto_full_matrix_${TS}"
mkdir -p "$LOG_DIR"

# ── Stage metadata (index → name, cost_cents) ─────────────────────────────
STAGE_NAMES=(_ build unit_x86 integration python bench_local aarch64_unit eks_full aarch64_unit_ssh)
STAGE_COSTS=(0 0    0        0           0      0          50           100    0)                    # cents

SELECTED=(1 2 8 3 4)                   # default: local matrix (+ parallel arm64 SSH)
KEEP_GOING=false
DRY_RUN=false
REPO_ARG=""
ARM64_SSH_ENABLED=true
FORCE_RESYNC=false

# ── Helpers ───────────────────────────────────────────────────────────────
CYAN='\033[0;36m'; GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
info() { echo -e "${GREEN}[INFO]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
step() { echo -e "\n${CYAN}══ Stage $1/$2: $3 ══${NC}"; }
die()  { echo -e "${RED}[ERR]${NC} $*" >&2; exit 1; }

usage() {
  cat <<EOF
Usage: $0 [options]

  --stages=LIST     Comma-separated stage numbers or names (default: 1,2,8,3,4)
                    Special: all, local (=1,2,8,3,4), eks (=1,2,8,3,4,7)
  --local           Shorthand for --stages=1,2,8,3,4
  --eks             Shorthand for --stages=1,2,8,3,4,7
  --all             Shorthand for --stages=1,2,8,3,4,5,7
  --keep-going      Don't fail-fast; run all selected stages
  --skip-build      Remove stage 1 from the plan
  --no-arm64        Opt out of stage 8 (persistent-Graviton SSH unit stage)
  --skip-arm64      Alias for --no-arm64
  --force-resync    Stage 8: force rsync (ignore last-synced-SHA cache), add --checksum
  --repo=URL        ECR repo for aarch64 image (stage 6)
  --dry-run         Print the plan without executing
  -h|--help         This help

Stages:
  1 build        2 unit_x86     3 integration  4 python
  5 bench_local  6 aarch64_unit (deprecated, use stage 8 instead)
  7 eks_full     8 aarch64_unit_ssh

Stage 8 uses a persistent Graviton EC2 instance (rsync + ssh + ninja + ctest)
and runs in parallel with stages 2, 3, and 4. Override the host with:
  GRAVITON_HOST=ec2-user@<ip>  GRAVITON_KEY=\$HOME/<key>.pem
If SSH preflight fails, stage 8 is skipped with a WARN (non-blocking).

Examples:
  $0 --local
  $0 --local --no-arm64
  $0 --eks --repo=<ecr>/zeptodb
  $0 --all --repo=<ecr>/zeptodb --dry-run
EOF
}

parse_stages() {
  local raw="$1"; local -a out=()
  IFS=',' read -ra parts <<< "$raw"
  for p in "${parts[@]}"; do
    case "$p" in
      all)   out+=(1 2 8 3 4 5 7) ;;
      local) out+=(1 2 8 3 4) ;;
      eks)   out+=(7) ;;
      build) out+=(1) ;; unit_x86) out+=(2) ;; integration) out+=(3) ;;
      python) out+=(4) ;; bench_local) out+=(5) ;; aarch64_unit) out+=(6) ;;
      eks_full) out+=(7) ;; aarch64_unit_ssh) out+=(8) ;;
      [1-8]) out+=("$p") ;;
      *) die "Unknown stage: $p" ;;
    esac
  done
  # dedupe + sort
  printf '%s\n' "${out[@]}" | sort -un | tr '\n' ' '
}

# ── CLI parsing ───────────────────────────────────────────────────────────
for arg in "$@"; do
  case "$arg" in
    --stages=*)  SELECTED=($(parse_stages "${arg#--stages=}")) ;;
    --local)     SELECTED=(1 2 8 3 4) ;;
    --eks)       SELECTED=(1 2 8 3 4 7) ;;
    --all)       SELECTED=(1 2 8 3 4 5 7) ;;
    --keep-going) KEEP_GOING=true ;;
    --skip-build) SELECTED=($(printf '%s\n' "${SELECTED[@]}" | grep -v '^1$' || true)) ;;
    --no-arm64|--skip-arm64) ARM64_SSH_ENABLED=false ;;
    --force-resync) FORCE_RESYNC=true ;;
    --repo=*)    REPO_ARG="${arg#--repo=}" ;;
    --dry-run)   DRY_RUN=true ;;
    -h|--help)   usage; exit 0 ;;
    *) die "Unknown flag: $arg (see --help)" ;;
  esac
done

# --no-arm64 strips stage 8 from whatever plan was assembled above.
if ! $ARM64_SSH_ENABLED; then
  SELECTED=($(printf '%s\n' "${SELECTED[@]}" | grep -v '^8$' || true))
fi

is_selected() { local n=$1; for s in "${SELECTED[@]}"; do [[ "$s" == "$n" ]] && return 0; done; return 1; }

# ── Wake/sleep coordination ──────────────────────────────────────────────
EKS_USED=false
if is_selected 6 || is_selected 7; then EKS_USED=true; fi

eks_sleep_done=false
eks_sleep() {
  $eks_sleep_done && return 0
  eks_sleep_done=true
  info "Sleeping EKS bench cluster (global trap)"
  "$REPO_ROOT/tools/eks-bench.sh" sleep || true
}
if $EKS_USED && ! $DRY_RUN; then trap eks_sleep EXIT INT TERM; fi

# ── Banner ───────────────────────────────────────────────────────────────
total_cents=0
plan_lines=()
for s in "${SELECTED[@]}"; do
  total_cents=$(( total_cents + STAGE_COSTS[s] ))
  plan_lines+=("  $s ${STAGE_NAMES[s]}")
done
cost_dollars=$(awk "BEGIN{printf \"%.2f\", $total_cents/100}")

echo -e "${CYAN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║         ZeptoDB Full Test Matrix Orchestrator           ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════╝${NC}"
echo "Plan:"
printf '%s\n' "${plan_lines[@]}"
echo "Selected: ${SELECTED[*]}"
echo "Estimated cost: \$${cost_dollars}"
echo "Fail-fast: $($KEEP_GOING && echo off || echo on)"
echo "Log dir: $LOG_DIR"
echo

if $DRY_RUN; then info "Dry-run: nothing executed."; exit 0; fi

# ── Stage runner ─────────────────────────────────────────────────────────
declare -a RESULTS_STAGE RESULTS_RC RESULTS_SEC
TOTAL_STAGES="${#SELECTED[@]}"
# devlog 094 #2/#8: when stages 2+8+3+4 are all selected they run as one
# parallel fork group (single "visible" step in the banner).
TOTAL_STAGES_VISIBLE="$TOTAL_STAGES"
_has() { printf '%s\n' "${SELECTED[@]}" | grep -qx "$1"; }
if _has 2 && _has 8 && _has 3 && _has 4; then
  TOTAL_STAGES_VISIBLE=$(( TOTAL_STAGES - 3 ))
elif _has 2 && _has 8; then
  TOTAL_STAGES_VISIBLE=$(( TOTAL_STAGES - 1 ))
fi
IDX=0

print_summary() {
  echo
  echo -e "${CYAN}── Summary ──${NC}"
  printf "  %-20s %-6s %s\n" "Stage" "Result" "Wall"
  printf "  %-20s %-6s %s\n" "-----" "------" "----"
  local any_fail=0
  for i in "${!RESULTS_STAGE[@]}"; do
    local st="${RESULTS_STAGE[i]}" rc="${RESULTS_RC[i]}" sec="${RESULTS_SEC[i]}"
    local status
    if [[ "$rc" == "0" ]]; then status="${GREEN}PASS${NC}"; else status="${RED}FAIL${NC}"; any_fail=1; fi
    printf "  %-20s %b   %ss\n" "$st" "$status" "$sec"
  done
  echo "  Logs: $LOG_DIR"
  return $any_fail
}

run_stage() {
  local n=$1 cmd="$2"
  IDX=$((IDX+1))
  local name="${STAGE_NAMES[n]}"
  local log="$LOG_DIR/stage_${n}_${name}.log"
  step "$IDX" "${TOTAL_STAGES_VISIBLE:-$TOTAL_STAGES}" "$name ($n)  →  $log"
  local t0 rc=0
  t0=$(date +%s)
  # Run through bash -c so compound commands work, tee to log.
  set +e
  ( cd "$REPO_ROOT" && bash -c "$cmd" ) 2>&1 | tee "$log"
  rc=${PIPESTATUS[0]}
  set -e
  local dt=$(( $(date +%s) - t0 ))
  RESULTS_STAGE+=("$n:$name")
  RESULTS_RC+=("$rc")
  RESULTS_SEC+=("$dt")
  if [[ $rc -ne 0 ]]; then
    echo -e "${RED}[FAIL]${NC} stage $n ($name) rc=$rc  (${dt}s)"
    if ! $KEEP_GOING; then print_summary; exit $rc; fi
  else
    echo -e "${GREEN}[OK]${NC}   stage $n ($name) (${dt}s)"
  fi
}

# ── Command table ────────────────────────────────────────────────────────
NPROC="$(nproc)"

cmd_build='cd build && ninja -j'"$NPROC"' zepto_tests test_feeds test_migration zepto_http_server zepto_data_node zepto-cli'

cmd_unit_x86='cd build && ctest -j'"$NPROC"' -E "Benchmark\.|K8s" --output-on-failure --timeout 180'

cmd_integration='set -e; found=0; for t in tests/integration/*.sh; do
    [[ -x "$t" ]] || continue
    found=$((found+1))
    echo "── $t ──"
    "$t"
  done
  [[ $found -gt 0 ]] || { echo "No executable integration scripts."; exit 0; }'

cmd_python='if command -v pytest >/dev/null 2>&1 && python3 -c "import zeptodb" 2>/dev/null; then
    pytest tests/python/ -x --timeout=120
  else
    echo "WARN: pytest or zeptodb module unavailable — skipping python stage."
  fi'

cmd_bench_local='ARCH_BENCH=tests/bench/run_arch_bench.sh
  if [[ -x "$ARCH_BENCH" ]] && "$ARCH_BENCH" --help 2>&1 | grep -q -- --local-only; then
    "$ARCH_BENCH" --skip-build --local-only
  else
    echo "run_arch_bench.sh has no --local-only — running local bench binaries briefly."
    # Smoke-level coverage only; use run_arch_bench.sh directly for real numbers.
    for b in bench_sql bench_pipeline bench_parallel; do
      if [[ -x "build/$b" ]]; then
        echo "── $b (15s cap) ──"
        timeout 15 "build/$b" || true
      fi
    done
  fi'

# Stage 6/7 pass --skip-wake when EKS is shared across stages.
A_REPO_ENV=""
if [[ -n "$REPO_ARG" ]]; then A_REPO_ENV="REPO='$REPO_ARG' "; fi

cmd_aarch64='WAKE_GUARD="$LOG_DIR/.eks_awake"
  if [[ ! -f "$WAKE_GUARD" ]]; then
    echo "── Waking EKS bench cluster (shared) ──"
    "$REPO_ROOT/tools/eks-bench.sh" wake
    touch "$WAKE_GUARD"
  else
    echo "── EKS already awake (shared from earlier stage) ──"
  fi
  # run-aarch64-tests.sh does its own wake+trap. We cannot easily skip that
  # trap without editing it, so simply re-wake is idempotent; the global trap
  # in this orchestrator owns the sleep.
  '"$A_REPO_ENV"'"$REPO_ROOT/tools/run-aarch64-tests.sh"'

cmd_eks_full='WAKE_GUARD="$LOG_DIR/.eks_awake"
  # Stage 6 (run-aarch64-tests.sh) installs its own EXIT trap that sleeps the
  # cluster when it finishes, so by the time stage 7 runs the cluster may be
  # asleep even though $WAKE_GUARD exists. Re-wake unconditionally (idempotent).
  "$REPO_ROOT/tools/eks-bench.sh" wake
  if [[ ! -f "$WAKE_GUARD" ]]; then
    touch "$WAKE_GUARD"
  fi
  "$REPO_ROOT/tests/k8s/run_eks_bench.sh" --skip-wake --keep'

# ── Stage 8: persistent-Graviton SSH unit stage (devlog 093, updated 094) ─
# Uses the same host/key pattern as .githooks/pre-push. Non-blocking skip on
# SSH failure so local-dev without VPN / with the host offline still passes.
GRAVITON_HOST="${GRAVITON_HOST:-ec2-user@172.31.71.135}"
GRAVITON_KEY="${GRAVITON_KEY:-$HOME/ec2-jinmp.pem}"
export GRAVITON_HOST GRAVITON_KEY FORCE_RESYNC

cmd_aarch64_ssh='set -e
  SSH_OPTS="-i $GRAVITON_KEY -o StrictHostKeyChecking=no"
  echo "── Graviton preflight: $GRAVITON_HOST (key=$GRAVITON_KEY) ──"
  if ! ssh -o ConnectTimeout=3 -o BatchMode=yes $SSH_OPTS "$GRAVITON_HOST" "uname -m" >/tmp/.graviton_arch.$$ 2>&1; then
    echo "WARN: SSH preflight to $GRAVITON_HOST failed — skipping arm64 stage (non-blocking)."
    cat /tmp/.graviton_arch.$$ || true; rm -f /tmp/.graviton_arch.$$
    exit 0
  fi
  echo "Remote arch: $(cat /tmp/.graviton_arch.$$)"; rm -f /tmp/.graviton_arch.$$

  # devlog 094 #3: skip rsync entirely when source tree is unchanged since last sync.
  CACHE_DIR="$HOME/.cache/zepto_matrix"
  mkdir -p "$CACHE_DIR"
  HOST_TAG=$(echo -n "$GRAVITON_HOST" | md5sum | cut -c1-8)
  SYNC_MARKER="$CACHE_DIR/last_sync_${HOST_TAG}"
  CUR_SHA=$(cd "$REPO_ROOT" && git rev-parse HEAD 2>/dev/null || echo dirty)
  DIRTY=$(cd "$REPO_ROOT" && git status --porcelain 2>/dev/null | head -1)
  RSYNC_EXTRA=""
  if [[ "${FORCE_RESYNC:-false}" == "true" ]]; then RSYNC_EXTRA="--checksum"; fi

  if [[ "${FORCE_RESYNC:-false}" != "true" && -z "$DIRTY" && "$CUR_SHA" != "dirty" && -f "$SYNC_MARKER" && "$(cat "$SYNC_MARKER")" == "$CUR_SHA" ]]; then
    echo "── Source tree unchanged since last sync ($CUR_SHA) — skipping rsync ──"
  else
    echo "── Rsync source tree → $GRAVITON_HOST:~/zeptodb/ ──"
    rsync -az --delete --info=stats2 --human-readable $RSYNC_EXTRA \
      --exclude=build/ --exclude=build_clang/ --exclude=.git/ \
      --exclude=node_modules/ --exclude=web/ --exclude=site/ \
      --exclude="*.egg-info/" --exclude=dist/ --exclude=CMakePresets.json \
      -e "ssh $SSH_OPTS" \
      "$REPO_ROOT/" "$GRAVITON_HOST:~/zeptodb/"
    if [[ -z "$DIRTY" && "$CUR_SHA" != "dirty" ]]; then
      echo "$CUR_SHA" > "$SYNC_MARKER"
    fi
  fi

  # devlog 094 #7: stage 8 uses --timeout 300 (vs stage 2 which uses 180)
  # because the Graviton dev box is 4-core vs the local 8-core developer box;
  # WorkerPool.WaitIdle has been observed to hit 180.02s on arm64.
  echo "── Remote build + ctest (same exclusion regex as stage 2, timeout 300) ──"
  ssh $SSH_OPTS "$GRAVITON_HOST" "
    set -e
    cd ~/zeptodb
    # devlog 097: auto-configure when build/ missing (e.g., compiler changed,
    # CMakeCache.txt purged). CMakeLists.txt soft-default picks clang-19.
    if [[ ! -f build/build.ninja ]]; then
      echo ── Remote: build/ missing or stale, running cmake configure ──
      cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    fi
    cd build
    ninja -j\$(nproc) zepto_tests test_feeds test_migration
    ctest -j\$(nproc) -E \"Benchmark\\.|K8s\" --output-on-failure --timeout 300
  "'

# ── Parallel helper: fork N stages as background subshells ──────────────
# devlog 094 #2/#8: generalized from the 2-way helper (devlog 093) to run an
# arbitrary set of selected stages (typically 2, 8, 3, 4) concurrently when
# they do not contend for the same ports/files. Each child writes its own
# log and records rc + wall-time to side-files; parent waits for all, adds
# results to RESULTS_*, and applies fail-fast on the first non-zero rc.
# Usage: run_stages_parallel_many <n1> <cmd1> <n2> <cmd2> ...
run_stages_parallel_many() {
  local -a ns=() cmds=()
  while [[ $# -gt 0 ]]; do
    ns+=("$1"); cmds+=("$2"); shift 2
  done
  IDX=$((IDX+1))
  local banner="PARALLEL: "
  local i n name
  for i in "${!ns[@]}"; do
    n="${ns[i]}"
    name="${STAGE_NAMES[n]}"
    banner+="$name ($n)"
    [[ $i -lt $(( ${#ns[@]} - 1 )) ]] && banner+=" ‖ "
  done
  step "$IDX" "$TOTAL_STAGES_VISIBLE" "$banner"

  _fork_one() {
    local n="$1" cmd="$2" log="$3" rcf="$4" tf="$5"
    local t0 rc=0
    t0=$(date +%s)
    set +e
    ( cd "$REPO_ROOT" && bash -c "$cmd" ) >"$log" 2>&1
    rc=$?
    set -e
    echo "$rc" >"$rcf"
    echo $(( $(date +%s) - t0 )) >"$tf"
  }

  local -a pids=() logs=() rcfs=() tfs=()
  local cmd log rcf tf
  for i in "${!ns[@]}"; do
    n="${ns[i]}"
    cmd="${cmds[i]}"
    name="${STAGE_NAMES[${ns[i]}]}"
    log="$LOG_DIR/stage_${n}_${name}.log"
    rcf="$LOG_DIR/.rc_${n}"
    tf="$LOG_DIR/.sec_${n}"
    logs+=("$log"); rcfs+=("$rcf"); tfs+=("$tf")
    _fork_one "$n" "$cmd" "$log" "$rcf" "$tf" &
    pids+=("$!")
  done
  echo "  forked pids: ${pids[*]} — waiting…"
  for pid in "${pids[@]}"; do wait "$pid" || true; done

  local first_bad=0
  local rc dt
  for i in "${!ns[@]}"; do
    n="${ns[i]}"
    name="${STAGE_NAMES[${ns[i]}]}"
    rc=$(cat "${rcfs[i]}"); dt=$(cat "${tfs[i]}")
    RESULTS_STAGE+=("$n:$name")
    RESULTS_RC+=("$rc")
    RESULTS_SEC+=("$dt")
    echo "── tail ${logs[i]} ──"; tail -n 8 "${logs[i]}" || true
    if [[ "$rc" -ne 0 ]]; then
      echo -e "${RED}[FAIL]${NC} stage $n ($name) rc=$rc  (${dt}s)"
      [[ $first_bad -eq 0 ]] && first_bad=$rc
    else
      echo -e "${GREEN}[OK]${NC}   stage $n ($name) (${dt}s)"
    fi
  done
  if [[ $first_bad -ne 0 ]] && ! $KEEP_GOING; then
    print_summary; exit $first_bad
  fi
}

# Back-compat 2-way wrapper (still used if only stages 2+8 are selected).
run_stages_parallel() {
  run_stages_parallel_many "$1" "$2" "$3" "$4"
}

# Make LOG_DIR & REPO_ROOT visible to subshells.
export LOG_DIR REPO_ROOT

# ── Execute plan ─────────────────────────────────────────────────────────
# devlog 094 #2/#8: when stages 2+8+3+4 are all selected (the --local default
# case), fork them as one 4-way parallel group. Stages 3 (integration) and 4
# (python) use their own processes and pick_free_port, so they don't collide
# with stage 2 (ctest) or stage 8 (remote Graviton). Stages 1/5/6/7 stay
# sequential since they either produce inputs (1) or contend for EKS (6/7).
PAR_CONSUMED=false
HAS_2=false; HAS_3=false; HAS_4=false; HAS_8=false
for s in "${SELECTED[@]}"; do
  [[ "$s" == "2" ]] && HAS_2=true
  [[ "$s" == "3" ]] && HAS_3=true
  [[ "$s" == "4" ]] && HAS_4=true
  [[ "$s" == "8" ]] && HAS_8=true
done
PAR4=false
if $HAS_2 && $HAS_8 && $HAS_3 && $HAS_4; then PAR4=true; fi

for s in "${SELECTED[@]}"; do
  case "$s" in
    1) run_stage 1 "$cmd_build" ;;
    2)
      if $PAR4; then
        run_stages_parallel_many \
          2 "$cmd_unit_x86" \
          8 "$cmd_aarch64_ssh" \
          3 "$cmd_integration" \
          4 "$cmd_python"
        PAR_CONSUMED=true
      elif $HAS_8; then
        run_stages_parallel 2 "$cmd_unit_x86" 8 "$cmd_aarch64_ssh"
        PAR_CONSUMED=true
      else
        run_stage 2 "$cmd_unit_x86"
      fi
      ;;
    3) $PAR_CONSUMED || run_stage 3 "$cmd_integration" ;;
    4) $PAR_CONSUMED || run_stage 4 "$cmd_python" ;;
    5) run_stage 5 "$cmd_bench_local" ;;
    6) run_stage 6 "$cmd_aarch64" ;;
    7) run_stage 7 "$cmd_eks_full" ;;
    8)
      if $PAR_CONSUMED; then
        : # already ran in the parallel fork group
      else
        run_stage 8 "$cmd_aarch64_ssh"
      fi
      ;;
  esac
done

# ── Summary ──────────────────────────────────────────────────────────────
print_summary
exit $?
