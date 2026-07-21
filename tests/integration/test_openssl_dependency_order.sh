#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"

checked=0
for name in \
    zepto_http_server \
    zepto_flight_server \
    zepto_data_node \
    zepto_ingest_node \
    zepto_action_outcome_soak \
    zepto-opcua-browse; do
    binary="$BUILD_DIR/$name"
    if [[ ! -x "$binary" ]]; then
        echo "FAIL: deployment executable is not built: $binary"
        exit 1
    fi
    dependencies="$(readelf -d "$binary" 2>/dev/null || true)"
    if ! grep -Eq 'lib(arrow|parquet)' <<<"$dependencies" ||
       ! grep -Eq 'lib(ssl|crypto)' <<<"$dependencies"; then
        continue
    fi

    provider_line="$(grep -n -m1 -E 'Shared library: \[lib(ssl|crypto)' \
        <<<"$dependencies" | cut -d: -f1)"
    pyarrow_line="$(grep -n -m1 -E 'Shared library: \[lib(arrow|parquet)' \
        <<<"$dependencies" | cut -d: -f1)"
    if [[ -z "$provider_line" || -z "$pyarrow_line" ||
          "$provider_line" -ge "$pyarrow_line" ]]; then
        echo "FAIL: system OpenSSL does not precede pyarrow in $binary"
        grep -E 'Shared library: \[lib(ssl|crypto|arrow|parquet)' \
            <<<"$dependencies" || true
        exit 1
    fi
    checked=$((checked + 1))
done

if [[ "$checked" -eq 0 ]]; then
    echo "FAIL: no OpenSSL + pyarrow executables found under $BUILD_DIR"
    exit 1
fi

echo "PASS: test_openssl_dependency_order ($checked executables)"
