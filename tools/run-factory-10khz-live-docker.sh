#!/usr/bin/env bash
# Run the P9 factory 10 KHz live proof against real local Docker deployments
# of InfluxDB and TimescaleDB plus a local ZeptoDB HTTP server.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

DURATION_SECONDS=60
RATE=10000
BATCH_SIZE=500
SYMBOLS=100
OUT_DIR=""
ZEPTO_PORT=18123
INFLUX_PORT=18086
TIMESCALE_PORT=15432
INFLUX_IMAGE="${INFLUX_IMAGE:-influxdb:2.7}"
TIMESCALE_IMAGE="${TIMESCALE_IMAGE:-timescale/timescaledb:2.15.3-pg16}"
KEEP_CONTAINERS=0
SKIP_PULL=0

usage() {
  cat <<'EOF'
Usage: tools/run-factory-10khz-live-docker.sh [options]

Options:
  --seconds N          run duration per system (default: 60)
  --rate N             target rows/sec (default: 10000)
  --batch-size N       rows per write batch (default: 500)
  --symbols N          distinct symbols (default: 100)
  --out DIR            output directory (default: bench-results/factory-10khz/live-<timestamp>)
  --zepto-port N       local ZeptoDB HTTP port (default: 18123)
  --influx-port N      local InfluxDB HTTP port (default: 18086)
  --timescale-port N   local TimescaleDB PostgreSQL port (default: 15432)
  --skip-pull          do not docker pull competitor images before run
  --keep-containers    leave competitor containers running after the run
  -h, --help           show this help

Environment:
  INFLUX_IMAGE         InfluxDB image (default: influxdb:2.7)
  TIMESCALE_IMAGE      TimescaleDB image (default: timescale/timescaledb:2.15.3-pg16)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --seconds) DURATION_SECONDS="${2:?missing --seconds value}"; shift 2 ;;
    --rate) RATE="${2:?missing --rate value}"; shift 2 ;;
    --batch-size) BATCH_SIZE="${2:?missing --batch-size value}"; shift 2 ;;
    --symbols) SYMBOLS="${2:?missing --symbols value}"; shift 2 ;;
    --out) OUT_DIR="${2:?missing --out value}"; shift 2 ;;
    --zepto-port) ZEPTO_PORT="${2:?missing --zepto-port value}"; shift 2 ;;
    --influx-port) INFLUX_PORT="${2:?missing --influx-port value}"; shift 2 ;;
    --timescale-port) TIMESCALE_PORT="${2:?missing --timescale-port value}"; shift 2 ;;
    --skip-pull) SKIP_PULL=1; shift ;;
    --keep-containers) KEEP_CONTAINERS=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 1 ;;
  esac
done

RUN_ID="factory$(date +%Y%m%d%H%M%S)"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/bench-results/factory-10khz/live-$RUN_ID}"
mkdir -p "$OUT_DIR"

ZEPTO_PID=""
INFLUX_CONTAINER="zepto-p9-influxdb-$RUN_ID"
TIMESCALE_CONTAINER="zepto-p9-timescaledb-$RUN_ID"

cleanup() {
  local ec=$?
  if [[ -n "$ZEPTO_PID" ]]; then
    kill "$ZEPTO_PID" 2>/dev/null || true
    wait "$ZEPTO_PID" 2>/dev/null || true
  fi
  if [[ "$KEEP_CONTAINERS" -eq 0 ]]; then
    docker rm -f "$INFLUX_CONTAINER" "$TIMESCALE_CONTAINER" >/dev/null 2>&1 || true
  fi
  exit "$ec"
}
trap cleanup EXIT INT TERM

wait_http() {
  local url="$1" name="$2"
  for _ in $(seq 1 90); do
    if curl -sf "$url" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  echo "$name did not become ready: $url" >&2
  return 1
}

wait_timescale() {
  for _ in $(seq 1 90); do
    if docker exec "$TIMESCALE_CONTAINER" pg_isready -U zepto -d factory >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  echo "TimescaleDB did not become ready" >&2
  return 1
}

cd "$REPO_ROOT"
if [[ ! -x ./build/zepto_http_server ]]; then
  cmake --build build --target zepto_http_server -j"$(nproc)"
fi

if [[ "$SKIP_PULL" -eq 0 ]]; then
  docker pull "$INFLUX_IMAGE"
  docker pull "$TIMESCALE_IMAGE"
fi

./build/zepto_http_server --port "$ZEPTO_PORT" --no-auth \
  >"$OUT_DIR/zeptodb-server.log" 2>&1 &
ZEPTO_PID=$!
wait_http "http://127.0.0.1:$ZEPTO_PORT/health" "ZeptoDB"

docker rm -f "$INFLUX_CONTAINER" "$TIMESCALE_CONTAINER" >/dev/null 2>&1 || true
docker run -d --name "$INFLUX_CONTAINER" \
  -p "$INFLUX_PORT:8086" \
  -e DOCKER_INFLUXDB_INIT_MODE=setup \
  -e DOCKER_INFLUXDB_INIT_USERNAME=zepto \
  -e DOCKER_INFLUXDB_INIT_PASSWORD=zepto-password \
  -e DOCKER_INFLUXDB_INIT_ORG=zepto \
  -e DOCKER_INFLUXDB_INIT_BUCKET=factory \
  -e DOCKER_INFLUXDB_INIT_ADMIN_TOKEN=zepto-token \
  "$INFLUX_IMAGE" >/dev/null
wait_http "http://127.0.0.1:$INFLUX_PORT/health" "InfluxDB"

docker run -d --name "$TIMESCALE_CONTAINER" \
  -p "$TIMESCALE_PORT:5432" \
  -e POSTGRES_USER=zepto \
  -e POSTGRES_PASSWORD=zepto-password \
  -e POSTGRES_DB=factory \
  "$TIMESCALE_IMAGE" >/dev/null
wait_timescale

COMMON_ARGS="--seconds $DURATION_SECONDS --rate $RATE --batch-size $BATCH_SIZE --symbols $SYMBOLS --run-id $RUN_ID"
WORKLOAD="python3 tools/factory-10khz-workload.py"

ZEPTO_BENCH_CMD="$WORKLOAD zeptodb --url http://127.0.0.1:$ZEPTO_PORT $COMMON_ARGS" \
INFLUX_BENCH_CMD="$WORKLOAD influxdb --url http://127.0.0.1:$INFLUX_PORT --org zepto --bucket factory --token zepto-token $COMMON_ARGS" \
TIMESCALE_BENCH_CMD="$WORKLOAD timescaledb --container $TIMESCALE_CONTAINER --user zepto --db factory $COMMON_ARGS" \
  tools/run-factory-10khz-competitor-bench.sh \
    --out "$OUT_DIR" \
    --seconds "$DURATION_SECONDS" \
    --require-competitors

cat >"$OUT_DIR/run-metadata.json" <<EOF
{
  "run_id": "$RUN_ID",
  "seconds": $DURATION_SECONDS,
  "rate": $RATE,
  "batch_size": $BATCH_SIZE,
  "symbols": $SYMBOLS,
  "influx_image": "$INFLUX_IMAGE",
  "timescale_image": "$TIMESCALE_IMAGE",
  "zeptodb_port": $ZEPTO_PORT,
  "influx_port": $INFLUX_PORT,
  "timescale_port": $TIMESCALE_PORT
}
EOF

echo "factory 10KHz live summary: $OUT_DIR/summary.jsonl"
