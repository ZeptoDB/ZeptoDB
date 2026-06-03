#!/usr/bin/env bash
# Factory 10 KHz competitor benchmark harness.
#
# This wrapper standardizes the P9 smart-factory proof run across ZeptoDB,
# InfluxDB, and TimescaleDB. It intentionally accepts competitor commands via
# env vars because local installations, Docker Compose stacks, and cloud
# endpoints differ across customer labs.
set -euo pipefail

OUT_DIR="bench-results/factory-10khz"
DURATION_SECONDS=600
ZEPTO_ONLY=0
REQUIRE_COMPETITORS=0

usage() {
  cat <<'EOF'
Usage: tools/run-factory-10khz-competitor-bench.sh [options]

Options:
  --out DIR                 result directory (default: bench-results/factory-10khz)
  --seconds N               duration passed to ZeptoDB bench (default: 600)
  --zepto-only              run only ZeptoDB, record competitors as skipped
  --require-competitors     fail if INFLUX_BENCH_CMD or TIMESCALE_BENCH_CMD is unset
  -h, --help                show this help

Environment:
  ZEPTO_BENCH_BIN           ZeptoDB bench binary (default: ./build/bench_ingest_scale)
  ZEPTO_BENCH_ARGS          additional args for bench_ingest_scale
  ZEPTO_BENCH_CMD           full ZeptoDB command; overrides ZEPTO_BENCH_BIN/ARGS
  INFLUX_BENCH_CMD          command that runs the matching InfluxDB workload
  TIMESCALE_BENCH_CMD       command that runs the matching TimescaleDB workload

Example:
  ZEPTO_BENCH_ARGS="--host 127.0.0.1 --port 8123 --pods 1 --threads 8 --batch-size 10" \
  INFLUX_BENCH_CMD="./vendor/influx/factory_10khz.sh" \
  TIMESCALE_BENCH_CMD="./vendor/timescale/factory_10khz.sh" \
  tools/run-factory-10khz-competitor-bench.sh --seconds 600 --require-competitors
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out) OUT_DIR="${2:?missing --out value}"; shift 2 ;;
    --seconds) DURATION_SECONDS="${2:?missing --seconds value}"; shift 2 ;;
    --zepto-only) ZEPTO_ONLY=1; shift ;;
    --require-competitors) REQUIRE_COMPETITORS=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 1 ;;
  esac
done

mkdir -p "$OUT_DIR"
SUMMARY="$OUT_DIR/summary.jsonl"
: > "$SUMMARY"

timestamp_ns() {
  date +%s%N
}

json_escape() {
  python3 -c 'import json,sys; print(json.dumps(sys.stdin.read())[1:-1])'
}

record() {
  local system="$1" status="$2" started_ns="$3" ended_ns="$4" log="$5"
  local escaped_log
  escaped_log="$(printf '%s' "$log" | json_escape)"
  printf '{"system":"%s","status":"%s","started_ns":%s,"ended_ns":%s,"log":"%s"}\n' \
    "$system" "$status" "$started_ns" "$ended_ns" "$escaped_log" >> "$SUMMARY"
}

run_case() {
  local system="$1" cmd="$2"
  local log_file="$OUT_DIR/${system}.log"
  local start end status
  start="$(timestamp_ns)"
  set +e
  bash -lc "$cmd" >"$log_file" 2>&1
  status=$?
  set -e
  end="$(timestamp_ns)"
  if [[ $status -eq 0 ]]; then
    record "$system" "pass" "$start" "$end" "$log_file"
  else
    record "$system" "fail:$status" "$start" "$end" "$log_file"
    return "$status"
  fi
}

if [[ -n "${ZEPTO_BENCH_CMD:-}" ]]; then
  run_case "zeptodb" "$ZEPTO_BENCH_CMD"
else
  ZEPTO_BENCH_BIN="${ZEPTO_BENCH_BIN:-./build/bench_ingest_scale}"
  ZEPTO_BENCH_ARGS="${ZEPTO_BENCH_ARGS:-}"
  if [[ ! -x "$ZEPTO_BENCH_BIN" ]]; then
    echo "ZeptoDB bench binary is not executable: $ZEPTO_BENCH_BIN" >&2
    exit 2
  fi
  run_case "zeptodb" "\"$ZEPTO_BENCH_BIN\" --seconds \"$DURATION_SECONDS\" $ZEPTO_BENCH_ARGS"
fi

if [[ $ZEPTO_ONLY -eq 1 ]]; then
  now="$(timestamp_ns)"
  record "influxdb" "skipped:zepto-only" "$now" "$now" ""
  record "timescaledb" "skipped:zepto-only" "$now" "$now" ""
elif [[ -n "${INFLUX_BENCH_CMD:-}" && -n "${TIMESCALE_BENCH_CMD:-}" ]]; then
  run_case "influxdb" "$INFLUX_BENCH_CMD"
  run_case "timescaledb" "$TIMESCALE_BENCH_CMD"
else
  if [[ $REQUIRE_COMPETITORS -eq 1 ]]; then
    echo "INFLUX_BENCH_CMD and TIMESCALE_BENCH_CMD are required" >&2
    exit 3
  fi
  now="$(timestamp_ns)"
  [[ -n "${INFLUX_BENCH_CMD:-}" ]] \
    && run_case "influxdb" "$INFLUX_BENCH_CMD" \
    || record "influxdb" "skipped:INFLUX_BENCH_CMD unset" "$now" "$now" ""
  [[ -n "${TIMESCALE_BENCH_CMD:-}" ]] \
    && run_case "timescaledb" "$TIMESCALE_BENCH_CMD" \
    || record "timescaledb" "skipped:TIMESCALE_BENCH_CMD unset" "$now" "$now" ""
fi

echo "summary: $SUMMARY"
