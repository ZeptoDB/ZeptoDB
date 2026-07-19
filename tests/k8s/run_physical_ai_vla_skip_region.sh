#!/bin/bash
# Run research-only Experiment 028 with the shared staged EKS runner.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ZEPTO_VLA_EXPERIMENT=028
export ZEPTO_VLA_PYTHON_SCRIPT=physical_ai_vla_skip_region.py
export ZEPTO_VLA_RESULT_STEM=physical_ai_vla_skip_region_028

exec "$SCRIPT_DIR/run_physical_ai_vla_closed_loop.sh" "$@"
