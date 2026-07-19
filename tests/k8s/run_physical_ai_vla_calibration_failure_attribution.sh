#!/bin/bash
# Run shadow-only Experiment 032 with the shared staged EKS runner.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ZEPTO_VLA_EXPERIMENT=032
export ZEPTO_VLA_PYTHON_SCRIPT=physical_ai_vla_calibration_failure_attribution.py
export ZEPTO_VLA_RESULT_STEM=physical_ai_vla_calibration_failure_attribution_032

exec "$SCRIPT_DIR/run_physical_ai_vla_closed_loop.sh" "$@"
