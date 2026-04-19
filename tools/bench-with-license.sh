#!/usr/bin/env bash
# Run any ZeptoDB bench/test binary with the bench Enterprise license loaded.
# Usage: ./tools/bench-with-license.sh ./build/bench_cluster [args...]
set -euo pipefail
LIC="${ZEPTODB_LICENSE_FILE:-$(dirname "$0")/../keys/bench.license}"
[[ -r "$LIC" ]] || { echo "License not found: $LIC" >&2; exit 1; }
export ZEPTODB_LICENSE_KEY="$(tr -d '\n\r ' < "$LIC")"
exec "$@"
